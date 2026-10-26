#include "ota_download.h"
#include "ota_notice.h"
#include "ota_meta.h"
#include "ota_image.h"
#include "http_image.h"
#include "utils_md5.h"
#include "w25q128.h"
#include "at24c02.h"
#include "esp_at.h"
#include "usart.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash_data[8192], eeprom[256], image[513], wire[1200];
static uint32_t now, wire_size, wire_used, erase_count, reset_count;
static int scenario, io_error, commit_error;
static uint16_t pending_pull;
static uint8_t prompt;
static uint8_t query_pending;
static EspAt_Response response;
static EspAt_DataHandler data_handler;
static EspAt_FrameHandler frame_handler;
static EspAt_LineHandler line_handler;
UART_HandleTypeDef huart1, huart3;

uint32_t HAL_GetTick(void) { return now; }
void NVIC_SystemReset(void) { ++reset_count; }
void Error_Handler(void) { assert(0); }
void OtaApp_Checkpoint(void) {}
uint8_t OtaApp_CanInstall(void) { return scenario != 8 || erase_count == 0U; }
uint8_t Cloud_PauseForOta(void) { return 1U; }
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *u, const uint8_t *p, uint16_t n, uint32_t t)
{ (void)u; (void)p; (void)n; (void)t; return HAL_OK; }
HAL_StatusTypeDef At24c02_Read(uint32_t a, uint8_t *p, uint32_t n)
{ assert(a+n<=sizeof(eeprom)); if(io_error) return HAL_ERROR; memcpy(p,eeprom+a,n); return HAL_OK; }
HAL_StatusTypeDef At24c02_Write(uint32_t a, const uint8_t *p, uint32_t n)
{
    assert(a+n<=sizeof(eeprom)); if(io_error) return HAL_ERROR;
    memcpy(eeprom+a,p,n);
    if(commit_error && a==5U && *p==0xA5U) return HAL_ERROR;
    return HAL_OK;
}
HAL_StatusTypeDef W25Q128_ReadJedecId(uint32_t *id) { *id=0xEF4018U; return HAL_OK; }
HAL_StatusTypeDef W25Q128_Read(uint32_t a, uint8_t *p, uint32_t n)
{ assert(a+n<=sizeof(flash_data)); memcpy(p,flash_data+a,n); return HAL_OK; }
HAL_StatusTypeDef W25Q128_EraseSector(uint32_t a)
{ assert(a%4096U==0U && a+4096U<=sizeof(flash_data)); assert(eeprom[5]==0U); ++erase_count; memset(flash_data+a,255,4096); return HAL_OK; }
HAL_StatusTypeDef W25Q128_Program(uint32_t a, const uint8_t *p, uint32_t n)
{ assert(a+n<=sizeof(flash_data) && n<=256U && a/256U==(a+n-1U)/256U); memcpy(flash_data+a,p,n); return HAL_OK; }
void EspAt_GetStats(EspAt_Stats *s)
{ memset(s,0,sizeof(*s)); if(scenario==9 && erase_count>0U) s->overflow=1U; }
uint8_t EspAt_HasStreamFault(void) { return 0U; }
uint8_t EspAt_StreamIdle(void) { return 1U; }
void EspAt_SetDataHandler(EspAt_DataHandler h) { data_handler=h; }
void EspAt_SetFrameHandler(EspAt_FrameHandler h) { frame_handler=h; }
void EspAt_SetLineHandler(EspAt_LineHandler h) { line_handler=h; }
void EspAt_ResetResponse(void) { response=ESP_AT_RESPONSE_NONE; prompt=0U; }
EspAt_Response EspAt_GetResponse(void) { return response; }
uint8_t EspAt_HasPrompt(void) { return prompt; }
HAL_StatusTypeDef EspAt_SetPassive(uint8_t b) { (void)b; return HAL_OK; }
HAL_StatusTypeDef EspAt_Send(const char *s)
{
    unsigned requested;
    response=ESP_AT_RESPONSE_OK;
    if(strncmp(s,"AT+CIPSEND=",11)==0) { prompt=1U; response=ESP_AT_RESPONSE_NONE; }
    if(strcmp(s,"AT+CIPRECVLEN?\r\n")==0) {
        query_pending=1U; response=ESP_AT_RESPONSE_NONE;
    }
    if(sscanf(s,"AT+CIPRECVDATA=%u",&requested)==1) pending_pull=(uint16_t)requested;
    return HAL_OK;
}
HAL_StatusTypeDef EspAt_SendBytes(const uint8_t *p, uint16_t n)
{
    assert(n>0U && memcmp(p,"GET /fw.bin?x=1 ",16)==0);
    response=ESP_AT_RESPONSE_SEND_OK; return HAL_OK;
}
static void Pump(void)
{
    uint16_t n=pending_pull, i;
    if(query_pending) {
        char line[48]; query_pending=0U;
        snprintf(line,sizeof(line),"+CIPRECVLEN:%lu",(unsigned long)(wire_size-wire_used));
        assert(line_handler); line_handler(line); response=ESP_AT_RESPONSE_OK;
    }
    if(!n) return;
    pending_pull=0U;
    if(n>wire_size-wire_used) n=(uint16_t)(wire_size-wire_used);
    assert(frame_handler && data_handler); frame_handler(n);
    for(i=0;i<n;++i) data_handler(wire[wire_used++]);
    response=ESP_AT_RESPONSE_OK;
}
static void ParserChecks(void)
{
    OtaNotice n; OtaMeta m, decoded; uint8_t raw[64];
    const char *bad="{\"cmd\":\"ota\",\"version\":\"x\",\"url\":\"https://x/a\",\"size\":12,\"md5\":\"00000000000000000000000000000000\"}";
    assert(OtaNotice_Parse((const uint8_t*)bad,(uint16_t)strlen(bad),&n)==OTA_NOTICE_INVALID);
    {
        static const char *invalid[] = {
            "{\"cmd\":\"ota\",\"cmd\":\"ota\"}",
            "{\"cmd\":\"ota\",\"extra\":1}",
            "{\"cmd\":\"ota\",\"size\":491521}",
            "{\"cmd\":\"ota\",\"size\":0}",
            "{\"cmd\":\"ota\",\"size\":-1}"
        };
        unsigned j;
        const char *valid="{\"cmd\":\"ota\",\"version\":\"0.4.1\",\"url\":\"http://host:8080/a.bin?x=1\",\"size\":513,\"md5\":\"00000000000000000000000000000000\"}";
        assert(OtaNotice_Parse((const uint8_t*)valid,(uint16_t)strlen(valid),&n)==OTA_NOTICE_VALID);
        assert(n.port==8080U && strcmp(n.path,"/a.bin?x=1")==0);
        for(j=0U;j<sizeof(invalid)/sizeof(invalid[0]);++j)
            assert(OtaNotice_Parse((const uint8_t*)invalid[j],(uint16_t)strlen(invalid[j]),&n)==OTA_NOTICE_INVALID);
        assert(OtaNotice_OnCommand((const uint8_t*)valid,(uint16_t)strlen(valid),"0.4.1") && !OtaNotice_Get());
        static const char *headers[] = {
            "HTTP/1.1 302 Found\r\nContent-Length: 513\r\n\r\n",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 513\r\nContent-Length: 513\r\n\r\n",
            "HTTP/1.1 200 OK\r\n\r\n"
        };
        for(j=0U;j<sizeof(headers)/sizeof(headers[0]);++j) {
            assert(HttpImage_Begin(513U)==HAL_OK);
            assert(HttpImage_Feed((const uint8_t*)headers[j],(uint16_t)strlen(headers[j]))==HAL_ERROR);
        }
    }
    memset(&m,0,sizeof(m)); strcpy(m.version,"0.4.1"); m.image_size=513U;
    assert(OtaMeta_Encode(&m,raw)==OTA_META_OK);
    assert(OtaMeta_Decode(raw,&decoded)==OTA_META_OK);
    raw[5]=0xA5U; assert(OtaMeta_Decode(raw,&decoded)==OTA_META_OK && decoded.pending==0xA5U);
    raw[5]=1U; assert(OtaMeta_Decode(raw,&decoded)==OTA_META_INVALID); raw[5]=0U;
    raw[10]^=1U; assert(OtaMeta_Decode(raw,&decoded)==OTA_META_INVALID);
    memset(raw,255,sizeof(raw)); assert(OtaMeta_Decode(raw,&decoded)==OTA_META_EMPTY);
    m.image_size=OTA_APP_MAX_SIZE+1U; assert(OtaMeta_Encode(&m,raw)==OTA_META_INVALID);
}
int main(int argc, char **argv)
{
    OtaMeta old, actual; OtaDownloadProgress progress;
    uint32_t msp=0x200024A8U, entry=0x08008009U, i;
    uint8_t digest[16]; char hex[33], json[360], header[160];
    assert(argc==2); scenario=atoi(argv[1]); ParserChecks();
    memset(flash_data,0x5A,sizeof(flash_data)); memset(eeprom,255,sizeof(eeprom));
    for(i=0;i<sizeof(image);++i) image[i]=(uint8_t)i;
    memcpy(image,&msp,4); memcpy(image+4,&entry,4);
    if(scenario==4) { entry=0x08000009U; memcpy(image+4,&entry,4); }
    utils_md5(image,sizeof(image),digest);
    for(i=0;i<16;++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    snprintf(json,sizeof(json),"{\"cmd\":\"ota\",\"version\":\"0.4.1\",\"url\":\"http://test.local/fw.bin?x=1\",\"size\":513,\"md5\":\"%s\"}",hex);
    snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n",scenario==3?514U:513U);
    wire_size=(uint32_t)strlen(header); memcpy(wire,header,wire_size); memcpy(wire+wire_size,image,sizeof(image)); wire_size+=sizeof(image);
    if(scenario==1) wire[wire_size-1]^=1U;
    memset(&old,0,sizeof(old)); old.image_size=8U; strcpy(old.version,"first-setup");
    assert(OtaMeta_WriteDraft(&old)==OTA_META_OK);
    if(scenario==2) eeprom[5]=0xA5U;
    if(scenario==5) io_error=1;
    if(scenario==6) commit_error=1;
    if(scenario==7) now=0xFFFFFE00U;
    assert(OtaNotice_OnCommand((const uint8_t*)json,(uint16_t)strlen(json),"0.4.0")==1U);
    assert(OtaNotice_Get());
    for(i=0;i<20000U && !reset_count;++i) {
        now+=10U; Pump(); OtaDownload_Task(); OtaDownload_GetProgress(&progress);
        if(progress.stage==OTA_PROGRESS_FAILED) break;
    }
    if(scenario==0 || scenario==7) {
        if(!reset_count) fprintf(stderr,"OTA unexpected stop: %s (stage=%d, bytes=%lu)\n",progress.error,(int)progress.stage,(unsigned long)progress.done);
        assert(reset_count==1U && eeprom[5]==0xA5U && memcmp(flash_data,image,sizeof(image))==0);
        assert(OtaMeta_Read(&actual)==OTA_META_OK && strcmp(actual.version,"0.4.1")==0);
    } else {
        assert(reset_count==0U && progress.stage==OTA_PROGRESS_FAILED);
        assert(eeprom[5]==((scenario==2 || scenario==6)?0xA5U:0U));
        if(scenario==2 || scenario==5) assert(erase_count==0U);
        if(scenario==1 || scenario==3 || scenario==4 || scenario==8 || scenario==9) {
            assert(OtaDownload_NeedsRecovery() && OtaDownload_ResetForRetry());
            assert(!OtaNotice_Get() && !OtaDownload_IsBusy());
        } else assert(!OtaDownload_NeedsRecovery());
    }
    printf("PASS OTA scenario=%d\n",scenario); return 0;
}
