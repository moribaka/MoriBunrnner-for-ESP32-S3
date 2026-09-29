/* Qualified M36 64B programming only. Same commands, page size and 40MHz
 * clock; reduce host idle/transaction overhead while keeping every ready,
 * error, timeout and cancellation check. Modes are fixed before a job. */
static unsigned s_gba_intel_speed_mode = 2u;
static bool s_gba_intel_speed_eligible;

unsigned burner_gba_intel_speed_get(void) { return s_gba_intel_speed_mode; }
esp_err_t burner_gba_intel_speed_set(unsigned mode)
{
    if (mode > 2u) return ESP_ERR_INVALID_ARG;
    s_gba_intel_speed_mode = mode;
    return ESP_OK;
}

/* One optional command cycle followed by complete, separately addressed
 * status reads. Each cycle releases cartridge CS/RD/WR. TX and RX do not
 * overlap. Appending reads does not change the flash command sequence. */
static esp_err_t burner_intel_status_packet(uint32_t word, uint16_t command,
    bool write_command, unsigned samples, uint16_t *values)
{
    if (!values || !samples || samples > 4u || word > 0xFFFFFFu) return ESP_ERR_INVALID_ARG;
    _Alignas(4) uint8_t tx[51] = {0}, rx[51] = {0};
    size_t base = write_command ? 11u : 0u;
    if (write_command) {
        uint16_t raw = burner_apply_d0d1_swap_on_write(command, s_cart_ctx.d0d1_swapped);
        const uint8_t prefix[11] = {0xFF,0,0,0,0x3B,0xBB,0,0,0x3A,0x3B,0x3F};
        memcpy(tx,prefix,sizeof(prefix));
        tx[1]=(uint8_t)word; tx[2]=(uint8_t)(word>>8); tx[3]=(uint8_t)(word>>16);
        tx[6]=(uint8_t)raw; tx[7]=(uint8_t)(raw>>8);
    }
    for (unsigned i=0;i<samples;++i) {
        const uint8_t read[10]={0xFF,0,0,0,0x3B,0x29,0xAB,0,0,0x3F};
        size_t at=base+i*10u;
        memcpy(tx+at,read,sizeof(read));
        tx[at+1]=(uint8_t)word; tx[at+2]=(uint8_t)(word>>8); tx[at+3]=(uint8_t)(word>>16);
    }
    esp_err_t err=burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0,tx,rx,base+samples*10u);
    if (err!=ESP_OK) return err;
    for(unsigned i=0;i<samples;++i) {
        size_t at=base+i*10u;
        uint16_t raw=(uint16_t)(rx[at+7] | ((uint16_t)rx[at+8]<<8));
        values[i]=burner_apply_d0d1_swap_on_read(raw,s_cart_ctx.d0d1_swapped);
    }
    return ESP_OK;
}

static esp_err_t burner_intel_fast_poll(uint32_t address, uint16_t command,
    uint32_t timeout_ms, uint16_t *status)
{
    if (!status || (address&1u) || address>=BURN_GBA_LINEAR_ADDR_BYTES) return ESP_ERR_INVALID_ARG;
    *status=0;
    if (!timeout_ms) return ESP_ERR_TIMEOUT;
    bool packet_command=command!=0u && s_gba_intel_speed_mode==2u;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    esp_err_t err=burner_cancel_poll();
    if (err!=ESP_OK) return err;
    if (command && !packet_command) {
        err=burner_bacon_gba_command_write_u16(address>>1,command);
        if (err!=ESP_OK) return err;
    }
    unsigned attempts=0;
    while(esp_timer_get_time()<deadline) {
        err=burner_cancel_poll();
        if(err!=ESP_OK) return err;
        uint16_t values[4];
        unsigned samples=s_gba_intel_speed_mode==2u && !packet_command ? 4u : 1u;
        err=burner_intel_status_packet(address>>1,command,packet_command,samples,values);
        packet_command=false;
        if(err!=ESP_OK) return err;
        for(unsigned i=0;i<samples;++i) {
            if(values[i]!=0xFFFFu && (values[i]&0x80u) && burner_gba_intel_status_has_error(values[i])) {
                *status=values[i]; return ESP_ERR_INVALID_RESPONSE;
            }
        }
        *status=values[samples-1];
        if(*status!=0xFFFFu && (*status&0x80u)) return ESP_OK;
        /* An initial short poll window removes the unconditional 50us
         * sleeps. Sustained busy still backs off and remains cancellable. */
        if(++attempts>=8u) esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_intel_wait_ready_optimized(uint32_t address, uint16_t command,
    uint32_t timeout_ms, uint16_t *status)
{
    uint64_t start=burner_gba_diag_now_us();
    esp_err_t err=burner_intel_fast_poll(address,command,timeout_ms,status);
    burner_gba_chis_diag_add_wait_ready(burner_gba_diag_now_us()-start);
    return err;
}
