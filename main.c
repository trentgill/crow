#include "ll/system.h"
#include "ll/debug_pin.h"
#include "ll/debug_usart.h"
#include "ll/status_led.h"
#include "syscalls.c" // printf() redirection
#include "lib/io.h"
#include "lib/events.h"
#include "ll/timers.h"
#include "lib/metro.h"
#include "lib/clock.h"
#include "lib/caw.h"
#include "lib/ii.h"
#include "ll/i2c_pullups.h" // i2c_hw_pullups_init
#include "ll/random.h"
#include "lib/lualink.h"
#include "lib/repl.h"
#include "usbd/usbd_cdc_interface.h" // CDC_main_init()
#include "lib/bootloader.h" // bootloader_enter(), bootloader_restart()
#include "lib/flash.h" // Flash_clear_user_script()
#include "stm32f7xx_it.h" // CPU_count;
#include "lib/telemetry.h"
#include "lib/crashreport.h" // crashreport_init(), crashreport_tick()


int main(void)
{
    system_init();
    Telemetry_init();

    // Debugging
    Debug_Pin_Init();
    Debug_USART_Init(); // ignored in TRACE mode
    // User-readable status led
    status_led_init();
    status_led_fast(LED_SLOW); // slow blink until USB connection goes live
    status_led_set(1); // set status to ON to show sign of life straight away

    printf("\n\nhi from crow!\n");
    crashreport_init(); // before REPL_init() which checks crashreport_skip_userscript()

    // Drivers
    int max_timers = Timer_Init();
    IO_Init( max_timers-2 ); // use second-last timer
    IO_Start(); // must start IO before running lua init() script
    events_init();
    Metro_Init( max_timers-2 ); // reserve 2 timers for USB & ADC
    clock_init( 100 ); // TODO how to pass it the timer?
    Caw_Init();
    CDC_clear_buffers();

    i2c_hw_pullups_init(); // enable GPIO for v1.1 hardware pullups
    ii_init( II_CROW );
    Random_Init();

    REPL_init( Lua_Init() );

    REPL_print_script_name();
    Lua_crowbegin();

    uint32_t last_tick = HAL_GetTick();
    while(1){
        CPU_count++;
        Telemetry_loop();
        U_PrintNow();
        switch( Caw_try_receive() ){ // true on pressing 'enter'
            case C_repl:        REPL_eval( Caw_get_read()
                                         , Caw_get_read_len()
                                         , Caw_send_luaerror
                                         ); break;
            case C_boot:        bootloader_enter(); break;
            case C_startupload: REPL_begin_upload(); break;
            case C_endupload:   REPL_upload(0); break;
            case C_flashupload: REPL_upload(1); break;
            case C_restart:     bootloader_restart(); break;
            case C_print:       REPL_print_script(); break;
            case C_version:     system_print_version(); break;
            case C_identity:    system_print_identity(); break;
            case C_killlua:     REPL_reset(); break;
            case C_flashclear:  REPL_clear_script(); break;
            case C_loadFirst:   REPL_default_script(); break;
            default: break; // 'C_none' does nothing
        }
        Random_Update();
        uint32_t time_now = HAL_GetTick(); // for running a 1ms-interval tick
        if( last_tick != time_now ){ // called on 1ms interval
            last_tick = time_now;
            clock_update(time_now);
            crashreport_tick(time_now);
            status_led_tick(time_now);
        }
        event_next(); // check/execute single event
        ii_follow_process(); // first: a leader may be waiting on our answer
        ii_leader_process();
        Caw_send_queued();
    }
}
