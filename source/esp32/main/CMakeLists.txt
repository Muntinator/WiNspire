# The cxlink wire protocol and the guest-side router live with the emulator so
# both ends compile the same code and cannot drift.
#
# Both are compiled HERE, not just added to the include path. The calculator side
# runs cxlink.c on ARM926 under Ndless, this side on Xtensa under FreeRTOS, and
# the only platform-dependent part is the transport, which cxlink.h splits out
# behind CxlinkHal. Shipping only the header here would link against nothing
# (that was a real bug: the first build of this firmware failed with undefined
# references to cxlink_encode and cxlink_decoder_push).
#
# bridge_net.c is the DHCP server and the classifier for frames arriving from the
# guest. It is deliberately freestanding (no lwIP, no ESP-IDF) so the host build
# can execute it: `make selftest` runs it, which is the only executable check of
# the DHCP path that exists in this tree.
#
# cxlink.c needs get_uticks(), which app_main's translation unit provides from
# esp_timer_get_time().
idf_component_register(
    SRCS "main.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../winspire/cxlink.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../winspire/bridge_net.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../winspire"
)
