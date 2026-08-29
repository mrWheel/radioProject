#include "ftp_service.h"
#include "ftp_server.h"
#include "radio_storage.h"
esp_err_t ftp_service_start(void){ftp_server_config_t c=FTP_SERVER_DEFAULT_CONFIG();c.base_path=RADIO_STORAGE_PATH;c.control_port=21;c.max_clients=2;return ftp_server_start(&c);}

