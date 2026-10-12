#pragma once
// MQTT's command DTOs mention ModbusRtuClient, which owns only this reference.
// No USB transport code is linked or executed by the MQTT boundary fixture.
class CdcAcmDevice;
