#pragma once
#define ESP_RETURN_ON_ERROR(expr,tag,message) do {const auto host_err=(expr);if(host_err!=ESP_OK)return host_err;} while(0)
