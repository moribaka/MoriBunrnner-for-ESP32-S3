"""Exercise GBABF's real detection scanner and prove exit-state isolation."""
from pathlib import Path
import subprocess, shutil, tempfile, re
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'main/burner/core/burn/burner_gba_lowlevel.c').read_text(encoding='utf-8')
header=(ROOT/'main/burner/core/burn/burner_gbabf_probe.h').read_text(encoding='utf-8')
def function(name):
    m=re.search(r'^(?:static )?esp_err_t '+name+r'\([^;{}]*\)\s*\{',source,re.M)
    assert m, name
    end=m.end(); depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[m.start():end]
types=source[source.index('typedef struct {\n    uint32_t word_addr;'):source.index('static const burner_gba_raw_id_step_t s_gba_raw_reset_amd_f0')]
prefix=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
typedef int esp_err_t;
typedef int burner_nor_cmdset_t;
typedef struct { int cmdset; } burner_nor_entry_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define BURNER_NOR_CMDSET_INTEL 2
#define BURNER_NOR_CMDSET_AMD 1
#define ESP_LOGI(...) ((void)0)
static burner_gbabf_probe_state_t s_gbabf;
static struct { bool d0d1_swapped; } s_cart_ctx;
static unsigned address_index, encoding, calls, first_cmdset, clears;
static bool intel_target, absent, bus_fault;
static esp_err_t probe_result;
static const burner_nor_entry_t intel_entry={2}, amd_entry={1};
static const burner_nor_entry_t *burner_nor_db_lookup_gba(const uint8_t id[8]) {
 return id[0]==0x89 ? &intel_entry : id[0]==1 ? &amd_entry : NULL;
}
static burner_nor_cmdset_t burner_nor_entry_cmdset(const burner_nor_entry_t *e) {return e->cmdset;}
static bool burner_gba_id_looks_like_rom_header(const uint8_t *id) {(void)id;return false;}
static bool burner_gba_id_matches_plain_rom_data(const uint8_t *id) {(void)id;return false;}
static void burner_gba_probe_amd_runtime_clear(void) {++clears;}
static esp_err_t burner_bacon_gba_probe_after_power_locked(uint8_t *id,uint32_t *bytes,
 uint32_t *sector,uint16_t *buffer,bool *cfi,bool chislink,bool gbabf) {
 assert(!chislink && gbabf); (void)id;
 s_gbabf.active=true; s_gbabf.amd_valid=true;
 *bytes=33554432; *sector=131072; *buffer=64; *cfi=true; return probe_result;
}
'''
bus=r'''
static esp_err_t burner_gba_raw_read_id_with_method(const burner_gba_raw_id_method_t *m,
 bool swapped,uint8_t id[8],bool *changed) {
 if (!calls++) first_cmdset=m->cmdset;
 memset(id,0,8); *changed=false;
 if(bus_fault) return ESP_FAIL;
 if(m->cmdset==2) {
  assert(m->reset_count==2 && m->reset[0].data==0x50 && m->reset[1].data==0xFF);
  assert(m->enter_id_count==1 && m->enter_id[0].data==0x90);
  if(intel_target && !absent) {id[0]=0x89;*changed=true;}
 } else {
  // Expected physical byte addresses from the extracted ROM, divided by 2.
  const unsigned byte_pairs[3][2]={{0xAAA,0x554},{0x1554,0xAAA},{0xAAAA,0x5554}};
  const unsigned values[4][3]={{0xAA,0x55,0x90},{0xA9,0x56,0x90},
   {0xAAA9,0x5556,0x9090},{0xAAAA,0x5555,0x9090}};
  assert(m->enter_id_count==3 && m->reset_count==1);
  assert(m->enter_id[2].data==0x90 || m->enter_id[2].data==0x9090);
  bool match=m->enter_id[0].word_addr==byte_pairs[address_index][0]/2 &&
   m->enter_id[1].word_addr==byte_pairs[address_index][1]/2 &&
   m->enter_id[2].word_addr==byte_pairs[address_index][0]/2;
  for(unsigned i=0;i<3;++i) match &= m->enter_id[i].data==values[encoding][i];
  if(match && swapped==((encoding==1 || encoding==2)) && !absent && !intel_target) {
   id[0]=1; *changed=true;
  }
 }
 return ESP_OK;
}
'''
cases=r'''
int main(void) {
 burner_gba_raw_id_scan_best_t best; uint32_t count;
 for(address_index=0;address_index<3;++address_index) for(encoding=0;encoding<4;++encoding) {
  calls=0; memset(&s_gbabf,0,sizeof(s_gbabf));
  assert(burner_gbabf_scan_id(&best,&count)==ESP_OK);
  assert(best.entry==&amd_entry && count && first_cmdset==2);
  assert(s_gbabf.amd_valid && s_gbabf.address_index==address_index && s_gbabf.command_index==encoding);
 }
 intel_target=true; calls=0;
 assert(burner_gbabf_scan_id(&best,&count)==ESP_OK && best.entry==&intel_entry && calls==1);
 assert(!s_gbabf.amd_valid);
 absent=true; calls=0; address_index=2; encoding=3;
 assert(burner_gbabf_scan_id(&best,&count)==ESP_OK && best.entry==NULL && count==0 && calls==25);
 bus_fault=true;
 assert(burner_gbabf_scan_id(&best,&count)==ESP_FAIL);
 uint8_t id[8]; uint32_t bytes,sector; uint16_t buffer; bool cfi;
 for(probe_result=0;probe_result<=1;++probe_result) {
  assert(burner_gbabf_gba_probe_locked(id,&bytes,&sector,&buffer,&cfi)==probe_result);
  assert(!s_gbabf.active && !s_gbabf.amd_valid);
 }
 assert(clears==2);
 puts("GBABF Intel-first, 12 matrix combinations, failure propagation and state isolation passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c'; exe=Path(tmp)/'test.exe'
    c.write_text(header+prefix+types+bus+function('burner_gbabf_scan_id')+
                 function('burner_gbabf_gba_probe_locked')+cases,encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
