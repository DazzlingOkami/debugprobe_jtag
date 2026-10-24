#include "probe_config.h"
#include "pico/binary_info.h"


#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)


void bi_decl_config()
{
#ifdef PROBE_RESET_PIN
    bi_decl(bi_1pin_with_name(PROBE_RESET_PIN, "PROBE RESET"));
#endif

#ifdef PROBE_PIN_SWCLK
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWCLK, "PROBE SWCLK"));
#endif

#ifdef PROBE_PIN_SWDIO
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDIO, "PROBE SWDIO"));
#endif

#ifdef PROBE_PIN_SWDI
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDI, "PROBE SWDI"));
#endif

#ifdef PROBE_PIN_SWDIOEN
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDIOEN, "PROBE SWDIOEN"));
#endif

#ifdef PROBE_CDC_UART
    bi_decl(bi_program_feature("PROBE UART INTERFACE " STR(PROBE_UART_INTERFACE)));
    bi_decl(bi_program_feature("PROBE UART BAUDRATE " STR(PROBE_UART_BAUDRATE)));
    bi_decl(bi_program_feature("PROBE UART TX PIN SWD " STR(PROBE_UART_TX_SWD_PIN)));
    bi_decl(bi_program_feature("PROBE UART TX PIN JTAG " STR(PROBE_UART_TX_JTAG_PIN)));
    bi_decl(bi_1pin_with_name(PROBE_UART_RX, "PROBE UART RX"));
#endif

#ifdef PROBE_UART_CTS
    bi_decl(bi_1pin_with_name(PROBE_UART_CTS, "PROBE UART CTS"));
#endif
#ifdef PROBE_UART_RTS
    bi_decl(bi_1pin_with_name(PROBE_UART_RTS, "PROBE UART RTS"));
#endif
#ifdef PROBE_UART_DTR
    bi_decl(bi_1pin_with_name(PROBE_UART_DTR, "PROBE UART DTR"));
#endif

}
