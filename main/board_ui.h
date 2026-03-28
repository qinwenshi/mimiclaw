#pragma once

#include "bus/message_bus.h"
#include "esp_err.h"

typedef enum {
    BOARD_UI_PHASE_BOOT = 0,
    BOARD_UI_PHASE_WIFI,
    BOARD_UI_PHASE_ONBOARDING,
    BOARD_UI_PHASE_READY,
    BOARD_UI_PHASE_WORKING,
    BOARD_UI_PHASE_REPLY,
    BOARD_UI_PHASE_ERROR,
} board_ui_phase_t;

esp_err_t board_ui_init(void);
void board_ui_set_phase(board_ui_phase_t phase, const char *title, const char *detail);
void board_ui_note_text(const char *title, const char *detail);
void board_ui_note_inbound(const mimi_msg_t *msg);
void board_ui_note_outbound(const mimi_msg_t *msg);
