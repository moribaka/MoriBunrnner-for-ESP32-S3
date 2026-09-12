#include <cassert>
#include <cstdio>
#include "../../main/music/music_player_lyra.cpp"

namespace lyra::audio {
static Status fake{};
static unsigned stop_count, play_count;
esp_err_t init(){fake.initialized=true;return ESP_OK;}
esp_err_t set_maximum_volume_percent(uint8_t){return ESP_OK;}
esp_err_t set_volume(uint8_t v){fake.volume_percent=v;return ESP_OK;}
esp_err_t play_from(const char *p,uint32_t ms,uint32_t){
    ++play_count;fake.playing=true;fake.paused=false;fake.eof=false;
    fake.position_ms=ms;fake.duration_ms=60000;fake.sample_rate=44100;
    std::snprintf(fake.path,sizeof(fake.path),"%s",p);return ESP_OK;
}
esp_err_t stop(){fake.playing=false;fake.path[0]=0;return ESP_OK;}
esp_err_t stop_and_wait(){++stop_count;return stop();}
esp_err_t deinit(){auto e=stop_and_wait();fake.initialized=false;return e;}
esp_err_t toggle_pause(){fake.paused=!fake.paused;return ESP_OK;}
esp_err_t seek(uint32_t ms){fake.position_ms=ms;return ESP_OK;}
Status status(){return fake;}
}
extern "C" bool usb_msc_tf_in_use_by_host(){return false;}
int main(){
    music_player_set_volume(1);
    assert(music_player_acquire_burn_priority()==ESP_OK);
    assert(music_player_play("music/test.mp3",1000)==ESP_ERR_INVALID_STATE);
    assert(!ready);
    music_player_release_burn_priority();
    assert(music_player_play("music/test.mp3",1000)==ESP_OK);
    lyra::audio::fake.position_ms=12345;
    assert(music_player_acquire_burn_priority()==ESP_OK);
    assert(lyra::audio::stop_count==1 && !lyra::audio::fake.playing && !ready);
    music_player_snapshot_t snap;
    music_player_get_snapshot(&snap);
    assert(snap.state==MUSIC_PLAYER_STATE_PAUSED && snap.elapsed_ms==12345 && snap.volume_percent==1);
    assert(music_player_acquire_burn_priority()==ESP_OK);
    assert(lyra::audio::stop_count==1);
    assert(music_player_play("other.mp3",1000)==ESP_ERR_INVALID_STATE);
    assert(music_player_play_smb("other.mp3",1000)==ESP_ERR_INVALID_STATE);
    assert(music_player_toggle_pause()==ESP_ERR_INVALID_STATE);
    assert(music_player_seek_relative(100)==ESP_ERR_INVALID_STATE);
    music_player_release_burn_priority();
    assert(music_player_toggle_pause()==ESP_ERR_INVALID_STATE);
    music_player_release_burn_priority();
    assert(lyra::audio::play_count==1); // No automatic resume.
    assert(music_player_toggle_pause()==ESP_OK);
    assert(ready && lyra::audio::fake.volume_percent==1);
    assert(lyra::audio::fake.position_ms==12345 && lyra::audio::play_count==2);
    assert(music_player_acquire_burn_priority()==ESP_OK);
    music_player_stop();
    music_player_get_snapshot(&snap);assert(snap.state==MUSIC_PLAYER_STATE_IDLE);
    music_player_release_burn_priority();
    assert(!burn_owners && !saved_pause);
    puts("Music burn ownership, nested release, entry rejection and manual resume passed");
}
