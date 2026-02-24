# Main component CMakeLists.txt

idf_component_register(
    SRCS 
        "main.c"
        "echoes.c"
        "synthesis.c"
        "ota.c"
	"startup.c"
	"espnow_mesh.c"
	"markov.c"
	"remote_config.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        driver 
	esp_adc
        nvs_flash 
	esp_wifi
        esp_http_client 
        app_update
	json
)
