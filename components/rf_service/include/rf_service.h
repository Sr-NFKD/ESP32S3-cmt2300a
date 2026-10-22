#pragma once
#include "esp_err.h"
#include "rf_protocol.h"

esp_err_t rf_service_init(bool radio_available);
esp_err_t rf_service_request(const rf_command_t *command);
void rf_service_status(rf_status_t *status);
// Suspend capture/monitoring before flash writes. Never resumes RF on reboot.
esp_err_t rf_service_pause_for_ota(void);
void rf_service_resume_after_ota(void);
// Stop RF activity and its scan timer; wake returns the radio to IDLE.
esp_err_t rf_service_sleep(bool sleeping);
