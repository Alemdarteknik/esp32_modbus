#include "inverter_gateway/network/wifi_provisioning.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "inverter_gateway/app/monitor_output.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/network/espnow_mesh.hpp"
#include "inverter_gateway/network/coordinator_link.hpp"

namespace inverter_gateway::network {
namespace {

constexpr char tag[] = "provisioning";

esp_err_t advertise_setup_page()
{
    ESP_RETURN_ON_ERROR(mdns_init(), tag, "initialize mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set("esp32"), tag, "set mDNS hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("Modbus inverter gateway"), tag,
                        "set mDNS instance name");
    return mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

constexpr char page_html[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Inverter gateway setup</title>
<style>
:root{color-scheme:light;--blue:#087cc1;--ink:#172033;--muted:#64748b;--line:#dbe3ec;--ok:#087f5b;--warn:#b45309}
*{box-sizing:border-box}body{font:15px system-ui,-apple-system,sans-serif;max-width:820px;margin:auto;padding:20px;background:#f4f7fb;color:var(--ink)}
h1{margin:4px 0 2px;font-size:27px}h2{font-size:18px;margin:0 0 14px}.lead{margin:0 0 20px;color:var(--muted)}
.card{background:#fff;padding:20px;border-radius:14px;margin:14px 0;box-shadow:0 3px 16px #18315312}.step{color:var(--blue);font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.08em}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}.wide{grid-column:1/-1}label{display:block;font-weight:600;margin:0 0 6px}
input,select,button{width:100%;padding:11px 12px;font:inherit;border:1px solid #b8c4d1;border-radius:8px;background:#fff;color:var(--ink)}
input:focus,select:focus{outline:3px solid #0ea5e933;border-color:var(--blue)}button{border:0;background:var(--blue);color:#fff;font-weight:650;cursor:pointer}button.secondary{background:#e8f3fa;color:#075985;border:1px solid #b9dff2}button:disabled{opacity:.55;cursor:not-allowed}
.hint{display:block;color:var(--muted);font-size:13px;margin-top:5px}.hidden{display:none!important}.inline{display:flex;gap:10px;align-items:end}.inline>div{flex:1}.inline>button{width:auto;min-width:120px}
.password-entry{display:grid;grid-template-columns:1fr auto;gap:8px}.password-entry button{width:82px}
.check{display:flex;gap:9px;align-items:center;font-weight:500}.check input{width:auto}.status-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.badge{padding:9px 10px;border-radius:8px;background:#f1f5f9}.badge.ok{background:#e8f8f1;color:var(--ok)}.badge.wait{background:#fff7ed;color:var(--warn)}
#message{white-space:pre-wrap;margin-top:12px;color:var(--muted)}details{border-top:1px solid var(--line);margin-top:16px;padding-top:12px}summary{cursor:pointer;font-weight:650}.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px}
@media(max-width:620px){body{padding:12px}.grid,.status-grid,.actions{grid-template-columns:1fr}.wide{grid-column:auto}.inline{display:block}.inline button{margin-top:8px;width:100%}}
</style></head><body>
<h1>Modbus inverter gateway</h1><p class="lead">Choose this device's job, connect it, then verify the installation.</p>
<form id="f" novalidate>
<section class="card"><div class="step">Step 1</div><h2>Installation</h2><div class="grid">
<div class="wide"><label for="topology">Inverter system</label><select id="topology" name="topology">
<option value="standalone_single_phase">One single-phase inverter</option><option value="standalone_native_three_phase">One three-phase inverter</option><option value="parallel_single_phase">Multiple inverters in one parallel group</option><option value="parallel_three_phase_groups">Parallel inverter groups across L1, L2 and L3</option><option value="parallel_native_three_phase">Multiple three-phase inverters in parallel</option></select></div>
<div id="role-box" class="wide"><label for="device_role">What is this ESP connected to?</label><select id="device_role" name="device_role"></select><small class="hint" id="role-hint"></small></div>
<div id="rating-box"><label for="output_rated_power_kw">Inverter rated output power</label><select id="output_rated_power_kw" name="output_rated_power_kw"><option value="3">3 kW</option><option value="4">4 kW</option><option value="5">5 kW</option><option value="6">6 kW</option><option value="8">8 kW</option><option value="12" selected>12 kW</option><option value="15">15 kW</option><option value="20">20 kW</option></select><small class="hint">Select the nominal power printed on this inverter. Default: 12 kW. Used for load percentage only; does not change inverter settings.</small></div>
<div id="pv-box"><label for="connected_pv_inputs">Connected PV arrays / inputs</label><select id="connected_pv_inputs" name="connected_pv_inputs"><option value="0">No PV connected</option><option value="1">1 PV input</option><option value="2" selected>2 PV inputs</option><option value="3">3 PV inputs</option><option value="4">4 PV inputs</option><option value="5">5 PV inputs</option><option value="6">6 PV inputs</option><option value="7">7 PV inputs</option><option value="8">8 PV inputs</option><option value="9">9 PV inputs</option><option value="10">10 PV inputs</option><option value="11">11 PV inputs</option><option value="12">12 PV inputs</option><option value="13">13 PV inputs</option><option value="14">14 PV inputs</option><option value="15">15 PV inputs</option><option value="16">16 PV inputs</option></select><small class="hint">Choose how many PV channels are physically connected to this inverter. Unused channels are not shown in live MQTT data.</small></div>
<div id="equipment-box"><label class="check"><input id="battery_installed" type="checkbox" checked> Battery installed</label><div id="bms-box"><label class="check"><input id="bms_connected" type="checkbox" checked> Battery uses BMS communication</label></div><label class="check"><input id="generator_installed" type="checkbox"> Generator installed</label><small class="hint">Unused battery, BMS, and generator measurements will not be polled or shown.</small></div>
<div id="count-box"><label for="expected_inverter_count">Total number of inverters</label><input id="expected_inverter_count" name="expected_inverter_count" type="number" min="2" max="9" value="2"><small class="hint">Includes the master inverter.</small></div>
<input name="expected_member_count" type="hidden" value="0">
<div id="member-box"><label for="logical_member_id">Slave number for this inverter</label><select id="logical_member_id" name="logical_member_id"></select><small class="hint">Each slave must have a different number.</small></div>
<div id="phase-box"><label for="phase">Phase for this inverter</label><select id="phase" name="phase"><option value="none" hidden>Not applicable</option><option value="parallel_single_phase" hidden>Parallel single phase</option><option value="phase_1">Phase 1 (L1)</option><option value="phase_2">Phase 2 (L2)</option><option value="phase_3">Phase 3 (L3)</option></select></div>
</div></section>

<section id="internet-card" class="card"><div class="step">Step 2</div><h2>Internet connection</h2>
<div class="inline"><div><label for="wifi_networks">Available Wi-Fi networks</label><select id="wifi_networks"><option value="">Select a network</option></select></div><button type="button" class="secondary" id="scan">Scan Wi-Fi</button></div>
<div id="scan-message" class="hint">A scan will start automatically.</div>
<label for="wifi_ssid">Wi-Fi network name</label><input id="wifi_ssid" name="wifi_ssid" maxlength="32" autocomplete="off"><small class="hint">Selecting a network above fills this automatically. You can also enter a hidden network.</small>
<label for="wifi_password">Wi-Fi password</label><div class="password-entry"><input id="wifi_password" name="wifi_password" maxlength="64" type="password" autocomplete="new-password"><button type="button" class="secondary" id="toggle_wifi_password" aria-controls="wifi_password" aria-pressed="false">Show</button></div><small class="hint" id="wifi-password-hint">Leave blank to keep the saved password.</small>
</section>

<section id="mqtt-card" class="card"><div class="step">Step 3</div><h2>MQTT server</h2>
<div class="grid"><div><label for="mqtt_uri">Server address</label><input id="mqtt_uri" name="mqtt_uri" maxlength="128" autocomplete="off"><small class="hint" id="mqtt-uri-hint">An IP address or hostname is enough; mqtt:// is added automatically.</small></div>
<div><label for="mqtt_port">Port</label><input id="mqtt_port" name="mqtt_port" type="number" min="1" max="65535" value="1883"><small class="hint">The standard MQTT port is 1883.</small></div></div>
<label class="check"><input id="mqtt_auth" type="checkbox"> This MQTT server requires a username and password</label>
<div id="mqtt-auth-fields" class="grid"><div><label for="mqtt_username">MQTT username</label><input id="mqtt_username" name="mqtt_username" maxlength="64" autocomplete="username"></div><div><label for="mqtt_password">MQTT password</label><input id="mqtt_password" name="mqtt_password" maxlength="64" type="password" autocomplete="new-password"><small class="hint" id="mqtt-password-hint">Leave blank to keep the saved password.</small></div></div>
</section>

<section id="parallel-card" class="card"><div class="step">Parallel communication</div><h2>Connect the inverter ESPs</h2><p class="hint">This wireless link is used only when multiple inverters operate in parallel. Discovery starts after saving.</p>
<label for="espnow_channel">Parallel radio channel</label><select id="espnow_channel" name="espnow_channel"><option value="0">Use the selected Wi-Fi channel</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option><option>13</option><option>14</option></select><small class="hint" id="channel-hint">All parallel inverter ESPs must use the same channel.</small></section>

<section class="card"><details><summary>Advanced settings</summary><div class="grid" style="margin-top:14px">
<div id="site-box" class="wide"><label for="site_id">Permanent installation ID</label><input id="site_id" name="site_id" maxlength="32"><small class="hint">Keep this ID for the lifetime of the installation. When replacing its ESP, enter the previous installation ID to keep MQTT topics unchanged. Disconnect the old ESP and revoke its broker access first.</small><label class="check"><input id="replacement_confirmed" type="checkbox"> Replacement: old ESP is disconnected; restore this installation and clear saved radio peers</label><small class="hint">Re-pair parallel nodes after replacement. For an inverter-only replacement, retain this ID, review the rating/settings, and save to restart connection verification. Do not replay old inverter commands.</small></div>
<div id="modbus-box"><label for="modbus_slave_address">Inverter Modbus address</label><input id="modbus_slave_address" name="modbus_slave_address" type="number" min="1" max="247" value="1"><small class="hint">Usually 1. Change only when the inverter is configured differently.</small></div>
<div id="prefix-box"><label for="mqtt_topic_prefix">MQTT topic prefix</label><input id="mqtt_topic_prefix" name="mqtt_topic_prefix" maxlength="64" value="inverter"></div>
<div class="wide"><label class="check"><input id="monitor_output_enabled" type="checkbox"> Show detailed ESP serial-monitor output</label><small class="hint">Shows information messages and Modbus TX/RX frames. Warnings and errors remain visible when this is off.</small></div>
</div></details><div class="actions"><button type="submit" id="save">Save and start connection test</button><button type="button" id="finalize">Verify and finish setup</button></div><div id="message"></div></section>
</form>

<section class="card"><div class="step">Connection check</div><h2>Current status</h2><div id="status" class="status-grid"></div><small class="hint">When every required connection remains ready, setup finishes automatically and the ESP restarts with its setup Wi-Fi disabled.</small></section>

<script>
const f=document.querySelector('#f'),statusBox=document.querySelector('#status'),message=document.querySelector('#message');
const $=id=>document.getElementById(id);let loadedSsid='',wifiPasswordSaved=false,mqttPasswordSaved=false,mqttUriSaved=false,scanStarted=false,selectedNetworkSecure=false,statusRefreshMs=3000;
const mqttRoles=['standalone_combined','internet_gateway','coordinator_gateway_combined'];
const meshRoles=['parallel_coordinator','parallel_member','coordinator_gateway_combined'];
const localRoles=['standalone_combined','parallel_coordinator','parallel_member','coordinator_gateway_combined'];
const roleChoices={
 standalone:[['standalone_combined','This ESP reads the inverter and connects to MQTT']],
 parallel:[['coordinator_gateway_combined','Master inverter ESP and MQTT gateway'],['parallel_coordinator','Master inverter ESP using a separate Internet Gateway'],['parallel_member','Slave inverter ESP'],['internet_gateway','Internet Gateway ESP only - no inverter']]
};
function visible(id,show){$(id).classList.toggle('hidden',!show)}
function setRoles(preferred){const parallel=$('topology').value.startsWith('parallel_'),choices=parallel?roleChoices.parallel:roleChoices.standalone,r=$('device_role');r.innerHTML=choices.map(x=>`<option value="${x[0]}">${x[1]}</option>`).join('');r.value=choices.some(x=>x[0]===preferred)?preferred:choices[0][0]}
function memberOptions(){const total=Number(f.elements.expected_inverter_count.value)||2,el=$('logical_member_id'),role=$('device_role').value,old=Number(el.value)||1;f.elements.expected_member_count.value=String(Math.max(0,total-1));el.innerHTML='';if(role!=='parallel_member'){el.add(new Option('Master / coordinator','0'));el.value='0';return}for(let i=1;i<total;i++)el.add(new Option(`Slave ${i}`,String(i)));el.value=String(Math.min(old,total-1))}
function updateEquipment(){const installed=$('battery_installed').checked;visible('bms-box',installed);$('bms_connected').disabled=!installed;if(!installed)$('bms_connected').checked=false}
function updateUi(rolePreference){const topology=$('topology').value,parallel=topology.startsWith('parallel_');if(rolePreference!==undefined)setRoles(rolePreference);else if(![...$('device_role').options].some(o=>o.value===$('device_role').value))setRoles();const role=$('device_role').value,usesMqtt=mqttRoles.includes(role),usesMesh=meshRoles.includes(role),hasInverter=localRoles.includes(role);visible('count-box',parallel);visible('member-box',role==='parallel_member');visible('phase-box',parallel&&topology==='parallel_three_phase_groups'&&hasInverter);visible('pv-box',hasInverter);visible('rating-box',hasInverter);visible('equipment-box',hasInverter);visible('parallel-card',parallel&&usesMesh);visible('internet-card',usesMqtt);visible('mqtt-card',usesMqtt);visible('site-box',role!=='parallel_member'&&role!=='internet_gateway');visible('modbus-box',hasInverter);visible('prefix-box',usesMqtt);f.elements.expected_inverter_count.value=parallel?Math.max(2,Number(f.elements.expected_inverter_count.value)||2):1;f.elements.expected_member_count.value=parallel?Number(f.elements.expected_inverter_count.value)-1:0;if(role!=='parallel_member')f.elements.logical_member_id.value=0;if(!hasInverter)f.elements.phase.value='none';else if(topology==='parallel_single_phase')f.elements.phase.value='parallel_single_phase';else if(topology!=='parallel_three_phase_groups')f.elements.phase.value='none';if(usesMesh&&!usesMqtt&&f.elements.espnow_channel.value==='0')f.elements.espnow_channel.value='1';$('channel-hint').textContent=usesMqtt?'The channel is filled from the selected Wi-Fi network. Configure every slave with this same channel.':'Use the same channel selected on the master inverter ESP.';memberOptions();updateEquipment();$('wifi_ssid').required=usesMqtt;$('mqtt_uri').required=usesMqtt&&!mqttUriSaved;$('mqtt_port').required=usesMqtt;$('mqtt_topic_prefix').required=usesMqtt;$('role-hint').textContent=role==='parallel_member'?'This node sends its inverter data to the master ESP.':role==='internet_gateway'?'This ESP connects the master inverter ESP to Wi-Fi and MQTT through the gateway cable.':usesMqtt?'This device sends the installation data to MQTT.':'This device reads the master inverter and forwards data to the Internet Gateway.';if(usesMqtt&&!scanStarted&&!loadedSsid)scanWifi()}
function updateAuth(){const enabled=$('mqtt_auth').checked;visible('mqtt-auth-fields',enabled);$('mqtt_username').disabled=!enabled;$('mqtt_password').disabled=!enabled}
function togglePassword(inputId,button){const input=$(inputId),show=input.type==='password';input.type=show?'text':'password';button.textContent=show?'Hide':'Show';button.setAttribute('aria-pressed',show?'true':'false')}
function friendlyError(value){const key=String(value||'').trim(),messages={bad_header:'Some setup information is missing. Review the form and save again.',role_missing:'Choose what this ESP is connected to.',topology_missing:'Choose the inverter system type.',invalid_counts:'Check the total number of inverters.',invalid_member_id:'Choose a valid and unique slave number.',invalid_pv_input_count:'Choose between 0 and 16 connected PV inputs.',invalid_battery_configuration:'BMS communication requires an installed battery.',site_id_required:'Enter an Installation ID using letters, numbers, hyphen or underscore.',wifi_required:'Select or enter a Wi-Fi network.',mqtt_required:'Enter the MQTT server address.',espnow_channel_invalid:'Choose a valid parallel radio channel.','Waiting for ESP-NOW coordinator':'Waiting for the master inverter ESP.','Waiting for coordinator-gateway UART link':'Waiting for the cable between the master ESP and Internet Gateway.','MQTT is not connected':'The MQTT server is not connected.','Local inverter Modbus has not responded':'The inverter has not responded yet.','NVS save failed':'The ESP could not save the configuration.'};if(key.startsWith('Waiting for ESP-NOW members:'))return 'Waiting for all configured inverter ESPs: '+key.split(':').pop().trim();return messages[key]||key||'Please review the setup and try again.'}
function composeMqttEndpoint(){const mqtt=$('mqtt_uri'),port=Number($('mqtt_port').value),raw=mqtt.value.trim();if(!Number.isInteger(port)||port<1||port>65535)return'Enter an MQTT port from 1 to 65535.';if(!raw)return'';try{const u=new URL(raw.includes('://')?raw:'mqtt://'+raw);if(!u.hostname)return'Enter a valid MQTT server address.';u.port=String(port);mqtt.value=u.toString();return''}catch(e){return'Enter a valid MQTT server address.'}}
function validateForm(){const role=$('device_role').value,usesMqtt=mqttRoles.includes(role),mqtt=$('mqtt_uri');if(!$('topology').value)return'Choose the inverter system type.';if(!role)return'Choose what this ESP is connected to.';if(usesMqtt&&!$('wifi_ssid').value.trim())return'Select or enter a Wi-Fi network.';if(usesMqtt&&!mqtt.value.trim()&&!mqttUriSaved)return'Enter the MQTT server address.';if(usesMqtt){const endpointProblem=composeMqttEndpoint();if(endpointProblem)return endpointProblem}if(usesMqtt&&!$('mqtt_topic_prefix').value.trim())return'Enter the MQTT topic prefix under Advanced settings.';if($('mqtt_auth').checked&&!$('mqtt_username').value.trim())return'Enter the MQTT username, or turn off MQTT authentication.';return''}
async function scanWifi(){scanStarted=true;$('scan').disabled=true;$('scan-message').textContent='Scanning nearby networks...';try{const r=await fetch('/api/wifi/scan',{cache:'no-store'});if(!r.ok)throw new Error(await r.text());const data=await r.json(),select=$('wifi_networks');select.innerHTML='<option value="">Select a network</option>';data.networks.forEach(n=>{const o=new Option(`${n.ssid}  (${n.rssi} dBm${n.secure?', secured':', open'})`,n.ssid);o.dataset.secure=n.secure?'1':'0';o.dataset.channel=n.channel;select.add(o)});const current=$('wifi_ssid').value;if([...select.options].some(o=>o.value===current)){select.value=current;selectedNetworkSecure=select.selectedOptions[0].dataset.secure==='1';if(meshRoles.includes($('device_role').value))$('espnow_channel').value=select.selectedOptions[0].dataset.channel}$('scan-message').textContent=data.networks.length?`${data.networks.length} network(s) found.`:'No networks found. Try scanning again or enter the name manually.'}catch(e){$('scan-message').textContent='Wi-Fi scan failed: '+e.message}finally{$('scan').disabled=false}}
$('wifi_networks').onchange=e=>{if(!e.target.value)return;$('wifi_ssid').value=e.target.value;selectedNetworkSecure=e.target.selectedOptions[0].dataset.secure==='1';if(meshRoles.includes($('device_role').value))$('espnow_channel').value=e.target.selectedOptions[0].dataset.channel;$('wifi_password').focus();$('wifi-password-hint').textContent=selectedNetworkSecure?'Enter the network password.':'This is an open network; no password is required.'};
$('scan').onclick=scanWifi;$('toggle_wifi_password').onclick=e=>togglePassword('wifi_password',e.currentTarget);$('mqtt_auth').onchange=updateAuth;$('battery_installed').onchange=updateEquipment;$('topology').onchange=()=>{setRoles();updateUi()};$('device_role').onchange=()=>updateUi();$('expected_inverter_count').oninput=memberOptions;
async function load(){try{const c=await(await fetch('/api/config',{cache:'no-store'})).json();Object.keys(c).forEach(k=>{if(f.elements[k]&&typeof c[k]!=='boolean')f.elements[k].value=c[k]});const savedRating=Number(c.output_rated_power_kw);const ratingSelect=$('output_rated_power_kw');if(savedRating>0){if(![...ratingSelect.options].some(o=>Number(o.value)===savedRating))ratingSelect.add(new Option(savedRating+' kW (saved)',String(savedRating)));ratingSelect.value=String(savedRating)}else ratingSelect.value='12';if(c.connected_pv_inputs===undefined)$('connected_pv_inputs').value='2';$('battery_installed').checked=c.battery_installed!==false;$('bms_connected').checked=c.bms_connected!==false;$('generator_installed').checked=!!c.generator_installed;mqttUriSaved=!!c.mqtt_uri_set;$('mqtt_uri').value='';$('mqtt_port').value='1883';loadedSsid=c.wifi_ssid||'';wifiPasswordSaved=!!c.wifi_password_set;mqttPasswordSaved=!!c.mqtt_password_set;statusRefreshMs=Number(c.status_refresh_ms)||statusRefreshMs;$('monitor_output_enabled').checked=!!c.monitor_output_enabled;$('topology').value=c.topology&&c.topology!=='unknown'?c.topology:'standalone_single_phase';setRoles(c.device_role);$('mqtt_auth').checked=!!c.mqtt_username||mqttPasswordSaved;updateAuth();updateUi(c.device_role);if(loadedSsid)$('scan-message').textContent='Saved network loaded. Press Scan Wi-Fi to choose another network.';$('wifi-password-hint').textContent=wifiPasswordSaved?'Password saved. Leave blank to keep it.':'Enter the Wi-Fi password if required.';$('mqtt-password-hint').textContent=mqttPasswordSaved?'Password saved. Leave blank to keep it.':'MQTT password is optional.';$('mqtt-uri-hint').textContent=mqttUriSaved?'Server address saved. Leave blank to keep it, or enter a replacement.':'Enter the MQTT server IP address or hostname.'}catch(e){message.textContent='Could not load the saved setup. Please refresh the page.';setRoles();updateUi();updateAuth()}}
function badge(label,ok,text){return `<div class="badge ${ok?'ok':'wait'}"><strong>${label}</strong><br>${text}</div>`}
async function refreshStatus(){try{const x=await(await fetch('/api/status',{cache:'no-store'})).json(),role=$('device_role').value,usesMqtt=mqttRoles.includes(role),hasInverter=localRoles.includes(role),needsUart=role==='parallel_coordinator'||role==='internet_gateway',needsCoordinator=role==='parallel_member',needsMembers=role==='parallel_coordinator'||role==='coordinator_gateway_combined',linkOk=(!needsUart&&!needsCoordinator)||(needsUart&&x.coordinator_uart_connected)||(needsCoordinator&&x.espnow_coordinator_found),membersOk=!needsMembers||x.members_found>=x.members_expected,ready=x.validation==='valid'&&(!usesMqtt||(x.station_connected&&x.mqtt_connected))&&(!hasInverter||x.inverter_responding)&&linkOk&&membersOk;statusBox.innerHTML=badge('Installation ID',true,x.site_id)+badge('Configuration',x.validation==='valid',x.validation==='valid'?'Saved':friendlyError(x.validation))+badge('Wi-Fi',!usesMqtt||x.station_connected,usesMqtt?(x.station_connected?'Connected':'Waiting'):'Not required')+badge('MQTT',!usesMqtt||x.mqtt_connected,usesMqtt?(x.mqtt_connected?'Connected':'Waiting'):'Not required')+badge('Inverter',!hasInverter||x.inverter_responding,hasInverter?(x.inverter_responding?'Responding':'Waiting'):'Not required')+badge('Parallel nodes',membersOk,needsMembers?`${x.members_found}/${x.members_expected} found`:'Managed by master')+badge('Gateway link',linkOk,needsUart?(x.coordinator_uart_connected?'Gateway cable connected':'Waiting for gateway cable'):needsCoordinator?(x.espnow_coordinator_found?'Master ESP found':'Searching for master ESP'):'Not required');$('finalize').disabled=!ready}catch(e){statusBox.textContent='Status temporarily unavailable.'}}
f.onsubmit=async e=>{e.preventDefault();const problem=validateForm();if(problem){message.textContent=problem;return}message.textContent='Saving configuration...';$('save').disabled=true;const role=$('device_role').value,usesMqtt=mqttRoles.includes(role),ssid=$('wifi_ssid').value.trim();if(usesMqtt&&selectedNetworkSecure&&ssid!==loadedSsid&&!$('wifi_password').value){message.textContent='Enter the password for the selected secured Wi-Fi network.';$('save').disabled=false;return}const o={};o.replacement_confirmed=$('replacement_confirmed').checked;new FormData(f).forEach((v,k)=>o[k]=['expected_inverter_count','expected_member_count','logical_member_id','modbus_slave_address','espnow_channel','connected_pv_inputs','output_rated_power_kw'].includes(k)?Number(v):v);o.phase=f.elements.phase.value||'none';o.battery_installed=$('battery_installed').checked;o.bms_connected=o.battery_installed&&$('bms_connected').checked;o.generator_installed=$('generator_installed').checked;o.monitor_output_enabled=$('monitor_output_enabled').checked;o.clear_wifi_password=usesMqtt&&!selectedNetworkSecure&&ssid!==loadedSsid;o.clear_mqtt_credentials=usesMqtt&&!$('mqtt_auth').checked;if(!$('mqtt_auth').checked)o.mqtt_username='';if(!usesMqtt){o.wifi_ssid='';o.mqtt_uri='';o.mqtt_username='';o.clear_mqtt_credentials=true}const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),15000);try{const r=await fetch('/api/config',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(o),signal:controller.signal});const text=await r.text();if(!r.ok)throw new Error(text);message.textContent='Saved. The ESP is restarting to test the connections. Reconnect to this setup page in a few seconds.'}catch(err){message.textContent=err.name==='AbortError'?'The ESP did not answer in time. Stay connected to its Wi-Fi and try again.':'Could not save. '+friendlyError(err.message);$('save').disabled=false}finally{clearTimeout(timer)}};
$('finalize').onclick=async()=>{message.textContent='Checking all required connections...';try{const r=await fetch('/api/finalize',{method:'POST'}),text=await r.text();if(!r.ok)throw new Error(text);message.textContent='Setup complete. The ESP is restarting in normal operation mode.'}catch(err){message.textContent='Setup is not ready. '+friendlyError(err.message)}};
load().finally(()=>{refreshStatus();setInterval(refreshStatus,statusRefreshMs)});
</script></body></html>)HTML";

template <std::size_t N>
bool copy_json_string(cJSON *root, const char *key, char (&destination)[N])
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr ||
        std::strlen(item->valuestring) >= N) return false;
    std::strncpy(destination, item->valuestring, N);
    destination[N - 1] = '\0';
    return true;
}

bool json_u8(cJSON *root, const char *key, std::uint8_t &destination)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 255) return false;
    destination = static_cast<std::uint8_t>(item->valueint);
    return true;
}

bool json_bool(cJSON *root, const char *key, bool &destination)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(item)) return false;
    destination = cJSON_IsTrue(item);
    return true;
}

template <std::size_t N>
bool normalize_mqtt_uri(char (&uri)[N])
{
    if (uri[0] == '\0' || std::strstr(uri, "://") != nullptr) return true;
    char normalized[N]{};
    const int written = std::snprintf(normalized, sizeof(normalized),
                                      "mqtt://%s", uri);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(normalized)) {
        return false;
    }
    std::strncpy(uri, normalized, N);
    uri[N - 1] = '\0';
    return true;
}

void send_json(httpd_req_t *request, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, body != nullptr ? body : "{}");
    if (body != nullptr) cJSON_free(body);
    cJSON_Delete(root);
}

esp_err_t send_conflict(httpd_req_t *request, const char *message)
{
    httpd_resp_set_status(request, "409 Conflict");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, message);
}

} // namespace

WifiProvisioning *WifiProvisioning::instance_ = nullptr;

WifiProvisioning::WifiProvisioning(app::SystemConfig &config) : config_(config) {}

esp_err_t WifiProvisioning::initialize()
{
    if (initialized_) return ESP_OK;
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    if (esp_netif_create_default_wifi_sta() == nullptr ||
        esp_netif_create_default_wifi_ap() == nullptr) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init);
    if (result != ESP_OK) return result;
    // SystemConfig is the single source of credentials. Avoid keeping a
    // second, hidden copy of the station password in the Wi-Fi driver NVS.
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, this);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, this);
    if (result != ESP_OK) return result;
    initialized_ = true;
    instance_ = this;
    return ESP_OK;
}

esp_err_t WifiProvisioning::start_station()
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    wifi_config_t station{};
    std::strncpy(reinterpret_cast<char *>(station.sta.ssid), config_.wifi_ssid,
                 sizeof(station.sta.ssid));
    std::strncpy(reinterpret_cast<char *>(station.sta.password), config_.wifi_password,
                 sizeof(station.sta.password));
    station.sta.threshold.authmode = config_.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), tag, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), tag, "set STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), tag, "start Wi-Fi");
    return esp_wifi_connect();
}

esp_err_t WifiProvisioning::start_radio(std::uint8_t channel)
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), tag, "set radio mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), tag, "start radio");
    if (channel != 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE),
                            tag, "set ESP-NOW channel");
    }
    return ESP_OK;
}

esp_err_t WifiProvisioning::start_portal()
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    std::uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap{};
    std::snprintf(reinterpret_cast<char *>(ap.ap.ssid), sizeof(ap.ap.ssid),
                  "%s%02X%02X%02X", app::setup_ap_ssid_prefix,
                  static_cast<unsigned>(mac[3]), static_cast<unsigned>(mac[4]),
                  static_cast<unsigned>(mac[5]));
    std::strncpy(reinterpret_cast<char *>(ap.ap.password), app::setup_ap_password,
                 sizeof(ap.ap.password));
    ap.ap.ssid_len = std::strlen(reinterpret_cast<char *>(ap.ap.ssid));
    ap.ap.channel = config_.profile.espnow_channel == 0
                        ? app::setup_ap_default_channel
                        : config_.profile.espnow_channel;
    ap.ap.max_connection = app::setup_ap_max_connections;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), tag, "set APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), tag, "set AP config");
    if (config_.wifi_ssid[0] != '\0') {
        wifi_config_t station{};
        std::strncpy(reinterpret_cast<char *>(station.sta.ssid), config_.wifi_ssid,
                     sizeof(station.sta.ssid));
        std::strncpy(reinterpret_cast<char *>(station.sta.password), config_.wifi_password,
                     sizeof(station.sta.password));
        station.sta.threshold.authmode = config_.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), tag, "set portal STA config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), tag, "start AP");
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr) return ESP_ERR_NOT_FOUND;
    esp_netif_ip_info_t ap_ip{};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ap_ip), tag,
                        "get AP address");
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
    esp_err_t dhcp_result = esp_netif_dhcps_stop(ap_netif);
    if (dhcp_result != ESP_OK &&
        dhcp_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return dhcp_result;
    }
    std::uint8_t offer_dns = 0x02;
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                               sizeof(offer_dns)),
        tag, "offer captive DNS");
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN,
                                               &dns),
                        tag, "set captive DNS");
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               const_cast<char *>(app::setup_page_url),
                               std::strlen(app::setup_page_url)),
        tag, "set captive portal URI");
    dhcp_result = esp_netif_dhcps_start(ap_netif);
    if (dhcp_result != ESP_OK &&
        dhcp_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return dhcp_result;
    }
    if (config_.wifi_ssid[0] != '\0') esp_wifi_connect();
    ESP_RETURN_ON_ERROR(start_http_server(), tag, "start HTTP server");
    ESP_RETURN_ON_ERROR(advertise_setup_page(), tag,
                        "advertise setup webpage");
    if (captive_dns_task_ == nullptr &&
        xTaskCreate(captive_dns_task, "captive_dns", 3072, this, 4,
                    &captive_dns_task_) != pdPASS) {
        captive_dns_task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    bool verification_pending = false;
    const esp_err_t pending_result =
        app::ConfigStore{}.load_verification_pending(verification_pending);
    if (pending_result != ESP_OK) {
        ESP_LOGW(tag, "Could not load setup-verification state: %s",
                 esp_err_to_name(pending_result));
    } else if (verification_pending && verification_task_ == nullptr &&
               xTaskCreate(verification_task, "setup_verify", 3072, this, 4,
                           &verification_task_) != pdPASS) {
        verification_task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(tag, "Setup AP %s, password %s, open %s", ap.ap.ssid,
             app::setup_ap_password, app::setup_page_url);
    return ESP_OK;
}

esp_err_t WifiProvisioning::start_lan_page()
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(start_http_server(), tag, "start LAN HTTP server");
    ESP_RETURN_ON_ERROR(advertise_setup_page(),
                        tag, "advertise setup webpage");
    ESP_LOGW(tag, "Configuration webpage available at %s on the local Wi-Fi network",
             app::setup_page_url);
    return ESP_OK;
}

void WifiProvisioning::set_mqtt_ready_probe(ReadyProbe probe, void *context)
{
    mqtt_ready_probe_ = probe;
    mqtt_ready_context_ = context;
}

void WifiProvisioning::set_inverter_ready_probe(ReadyProbe probe, void *context)
{
    inverter_ready_probe_ = probe;
    inverter_ready_context_ = context;
}

void WifiProvisioning::set_mesh(EspNowMesh *mesh)
{
    mesh_ = mesh;
}

void WifiProvisioning::set_coordinator_link(CoordinatorLink *link)
{
    coordinator_link_ = link;
}

void WifiProvisioning::wifi_event(void *argument, esp_event_base_t base,
                                  std::int32_t event_id, void *event_data)
{
    auto *self = static_cast<WifiProvisioning *>(argument);
    if (self == nullptr) return;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->station_connected_.store(false);
        if (self->config_.wifi_ssid[0] != '\0' &&
            !self->suppress_station_reconnect_.load()) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->station_connected_.store(true);
        const auto *got_ip = static_cast<const ip_event_got_ip_t *>(event_data);
        if (got_ip != nullptr) {
            ESP_LOGW(tag, "Wi-Fi ready: IP=" IPSTR " gateway=" IPSTR,
                     IP2STR(&got_ip->ip_info.ip),
                     IP2STR(&got_ip->ip_info.gw));
        }
    }
}

esp_err_t WifiProvisioning::start_http_server()
{
    if (server_ != nullptr) return ESP_OK;
    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.stack_size = app::setup_http_task_stack_size;
    configuration.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&server_, &configuration), tag, "httpd_start");
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler, .user_ctx = this},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler, .user_ctx = this},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = this},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler, .user_ctx = this},
        {.uri = "/api/finalize", .method = HTTP_POST, .handler = finalize_handler, .user_ctx = this},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = this},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = this},
    };
    for (const auto &handler : handlers) ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &handler), tag, "register URI");
    return ESP_OK;
}

esp_err_t WifiProvisioning::captive_redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "Opening the inverter gateway setup page");
}

void WifiProvisioning::captive_dns_task(void *argument)
{
    auto *self = static_cast<WifiProvisioning *>(argument);
    const int socket_fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(tag, "Could not create captive DNS socket");
        if (self != nullptr) self->captive_dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    int reuse = 1;
    lwip_setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) != 0) {
        ESP_LOGE(tag, "Could not bind captive DNS socket");
        lwip_close(socket_fd);
        if (self != nullptr) self->captive_dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    std::array<std::uint8_t, 512> packet{};
    esp_netif_ip_info_t ap_ip{};
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr || esp_netif_get_ip_info(ap_netif, &ap_ip) != ESP_OK) {
        ESP_LOGE(tag, "Could not read captive portal address");
        lwip_close(socket_fd);
        if (self != nullptr) self->captive_dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        sockaddr_storage source{};
        socklen_t source_length = sizeof(source);
        const int received = lwip_recvfrom(
            socket_fd, packet.data(), packet.size(), 0,
            reinterpret_cast<sockaddr *>(&source), &source_length);
        if (received < 12 || (packet[2] & 0x80U) != 0 ||
            (packet[4] == 0 && packet[5] == 0)) continue;

        std::size_t cursor = 12;
        bool valid_name = false;
        while (cursor < static_cast<std::size_t>(received)) {
            const std::uint8_t label_length = packet[cursor++];
            if (label_length == 0) {
                valid_name = true;
                break;
            }
            if ((label_length & 0xc0U) != 0 || label_length > 63 ||
                cursor + label_length > static_cast<std::size_t>(received)) {
                break;
            }
            cursor += label_length;
        }
        if (!valid_name || cursor + 4 > static_cast<std::size_t>(received) ||
            cursor + 4 + 16 > packet.size()) continue;
        const std::uint16_t query_type =
            static_cast<std::uint16_t>(packet[cursor] << 8U) | packet[cursor + 1];
        cursor += 4;

        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[4] = 0;
        packet[5] = 1;
        packet[6] = 0;
        packet[7] = query_type == 1 ? 1 : 0;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        if (query_type == 1) {
            std::uint8_t answer[] = {
                0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x3c, 0x00, 0x04,
                0, 0, 0, 0,
            };
            std::memcpy(answer + 12, &ap_ip.ip.addr, 4);
            std::memcpy(packet.data() + cursor, answer, sizeof(answer));
            cursor += sizeof(answer);
        }
        lwip_sendto(socket_fd, packet.data(), cursor, 0,
                    reinterpret_cast<const sockaddr *>(&source), source_length);
    }
}

esp_err_t WifiProvisioning::page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page_html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiProvisioning::config_get_handler(httpd_req_t *request)
{
    auto *self = static_cast<WifiProvisioning *>(request->user_ctx);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_role", app::device_role_name(self->config_.profile.device_role));
    cJSON_AddStringToObject(root, "topology", app::topology_name(self->config_.profile.topology));
    cJSON_AddStringToObject(root, "phase", app::phase_assignment_name(self->config_.profile.phase));
    cJSON_AddNumberToObject(root, "expected_inverter_count", self->config_.profile.expected_inverter_count);
    cJSON_AddNumberToObject(root, "expected_member_count", self->config_.profile.expected_member_count);
    cJSON_AddNumberToObject(root, "logical_member_id", self->config_.profile.logical_member_id);
    cJSON_AddNumberToObject(root, "modbus_slave_address", self->config_.profile.modbus_slave_address);
    cJSON_AddNumberToObject(root, "espnow_channel", self->config_.profile.espnow_channel);
    cJSON_AddNumberToObject(root, "output_rated_power_kw", self->config_.output_rated_power_kw);
    cJSON_AddNumberToObject(root, "connected_pv_inputs",
                            self->config_.profile.connected_pv_inputs);
    cJSON_AddBoolToObject(root, "battery_installed",
                          self->config_.profile.battery_installed);
    cJSON_AddBoolToObject(root, "bms_connected",
                          self->config_.profile.bms_connected);
    cJSON_AddBoolToObject(root, "generator_installed",
                          self->config_.profile.generator_installed);
    cJSON_AddStringToObject(root, "site_id", self->config_.site_id);
    cJSON_AddStringToObject(root, "wifi_ssid", self->config_.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", "");
    cJSON_AddBoolToObject(root, "wifi_password_set", self->config_.wifi_password[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_uri", "");
    cJSON_AddBoolToObject(root, "mqtt_uri_set",
                          self->config_.mqtt_uri[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_username", self->config_.mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_password", "");
    cJSON_AddBoolToObject(root, "mqtt_password_set", self->config_.mqtt_password[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", self->config_.mqtt_topic_prefix);
    cJSON_AddBoolToObject(root, "monitor_output_enabled",
                          app::monitor_output_enabled());
    cJSON_AddNumberToObject(root, "status_refresh_ms",
                            app::setup_status_refresh_ms);
    send_json(request, root);
    return ESP_OK;
}

esp_err_t WifiProvisioning::receive_json(httpd_req_t *request, char *buffer,
                                         std::size_t capacity)
{
    if (request->content_len <= 0 || static_cast<std::size_t>(request->content_len) >= capacity) return ESP_ERR_INVALID_SIZE;
    int received = 0;
    while (received < request->content_len) {
        const int result = httpd_req_recv(request, buffer + received, request->content_len - received);
        if (result <= 0) return ESP_FAIL;
        received += result;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

esp_err_t WifiProvisioning::config_post_handler(httpd_req_t *request)
{
    auto *self = static_cast<WifiProvisioning *>(request->user_ctx);
    std::array<char, app::setup_request_max_bytes> buffer{};
    const esp_err_t receive_result = self->receive_json(request, buffer.data(),
                                                        buffer.size());
    if (receive_result != ESP_OK) {
        ESP_LOGW(tag, "Rejected setup save: invalid request body (%s)",
                 esp_err_to_name(receive_result));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid body");
    }
    cJSON *root = cJSON_Parse(buffer.data());
    app::SystemConfig candidate = self->config_;
    app::DeviceRole role{};
    app::SystemTopology topology{};
    app::PhaseAssignment phase{};
    const cJSON *role_json = cJSON_GetObjectItemCaseSensitive(root, "device_role");
    const cJSON *topology_json = cJSON_GetObjectItemCaseSensitive(root, "topology");
    const cJSON *phase_json = cJSON_GetObjectItemCaseSensitive(root, "phase");
    const cJSON *monitor_json =
        cJSON_GetObjectItemCaseSensitive(root, "monitor_output_enabled");
    const cJSON *mqtt_uri_json =
        cJSON_GetObjectItemCaseSensitive(root, "mqtt_uri");
    bool valid = root != nullptr && cJSON_IsString(role_json) && cJSON_IsString(topology_json) &&
                 app::parse_device_role(role_json->valuestring, role) &&
                 app::parse_topology(topology_json->valuestring, topology) &&
                 cJSON_IsString(phase_json) &&
                 app::parse_phase_assignment(phase_json->valuestring, phase) &&
                 cJSON_IsString(mqtt_uri_json) &&
                 cJSON_IsBool(monitor_json);
    const bool monitor_output_enabled = cJSON_IsTrue(monitor_json);
    if (valid) {
        const auto previous_profile = candidate.profile;
        char previous_site[sizeof(candidate.site_id)]{};
        char previous_wifi_ssid[sizeof(candidate.wifi_ssid)]{};
        std::strncpy(previous_site, candidate.site_id, sizeof(previous_site));
        std::strncpy(previous_wifi_ssid, candidate.wifi_ssid,
                     sizeof(previous_wifi_ssid));
        candidate.profile.device_role = role;
        candidate.profile.topology = topology;
        candidate.profile.phase = phase;
        candidate.profile.inverter_role = role == app::DeviceRole::parallel_member
                                              ? app::InverterRole::member
                                              : app::role_is_espnow_coordinator(role)
                                                    ? app::InverterRole::host
                                                    : role == app::DeviceRole::standalone_combined
                                                          ? app::InverterRole::standalone
                                                          : app::InverterRole::none;
        valid = json_u8(root, "expected_inverter_count", candidate.profile.expected_inverter_count) &&
                json_u8(root, "expected_member_count", candidate.profile.expected_member_count) &&
                json_u8(root, "logical_member_id", candidate.profile.logical_member_id) &&
                json_u8(root, "modbus_slave_address", candidate.profile.modbus_slave_address) &&
                json_u8(root, "espnow_channel", candidate.profile.espnow_channel) &&
                json_u8(root, "connected_pv_inputs", candidate.profile.connected_pv_inputs) &&
                json_bool(root, "battery_installed", candidate.profile.battery_installed) &&
                json_bool(root, "bms_connected", candidate.profile.bms_connected) &&
                json_bool(root, "generator_installed", candidate.profile.generator_installed) &&
                copy_json_string(root, "site_id", candidate.site_id) &&
                copy_json_string(root, "wifi_ssid", candidate.wifi_ssid) &&
                copy_json_string(root, "mqtt_username", candidate.mqtt_username) &&
                copy_json_string(root, "mqtt_topic_prefix", candidate.mqtt_topic_prefix);
        const cJSON *rating = cJSON_GetObjectItemCaseSensitive(root, "output_rated_power_kw");
        if (rating != nullptr) {
            if (!cJSON_IsNumber(rating) || !(rating->valuedouble >= 0.0 && rating->valuedouble <= 1000.0)) {
                valid = false;
            } else candidate.output_rated_power_kw = static_cast<float>(rating->valuedouble);
        }
        if (valid && mqtt_uri_json->valuestring[0] != '\0') {
            valid = copy_json_string(root, "mqtt_uri", candidate.mqtt_uri);
        }
        if (valid && !app::role_uses_mqtt(role)) {
            candidate.mqtt_uri[0] = '\0';
        }
        if (valid && candidate.site_id[0] == '\0') {
            valid = app::ensure_hardware_site_id(candidate) == ESP_OK;
        }
        if (valid && app::role_uses_mqtt(role)) {
            valid = normalize_mqtt_uri(candidate.mqtt_uri);
        }
        const bool pairing_identity_changed =
            previous_profile.device_role != candidate.profile.device_role ||
            previous_profile.topology != candidate.profile.topology ||
            previous_profile.logical_member_id != candidate.profile.logical_member_id ||
            previous_profile.espnow_channel != candidate.profile.espnow_channel ||
            std::strcmp(previous_site, candidate.site_id) != 0;
        if (valid && (pairing_identity_changed || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "replacement_confirmed")))) {
            candidate.peers.fill({});
            candidate.provisioned = false;
        }
        const cJSON *wifi_password = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
        const cJSON *mqtt_password = cJSON_GetObjectItemCaseSensitive(root, "mqtt_password");
        const bool clear_wifi_password = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(root, "clear_wifi_password"));
        const bool clear_mqtt_credentials = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(root, "clear_mqtt_credentials"));
        if (valid && (clear_wifi_password ||
                      (std::strcmp(previous_wifi_ssid, candidate.wifi_ssid) != 0 &&
                       (!cJSON_IsString(wifi_password) || wifi_password->valuestring[0] == '\0')))) {
            candidate.wifi_password[0] = '\0';
        }
        if (valid && clear_mqtt_credentials) {
            candidate.mqtt_username[0] = '\0';
            candidate.mqtt_password[0] = '\0';
        }
        if (valid && cJSON_IsString(wifi_password) && wifi_password->valuestring[0]) valid = copy_json_string(root, "wifi_password", candidate.wifi_password);
        if (valid && !clear_mqtt_credentials && cJSON_IsString(mqtt_password) &&
            mqtt_password->valuestring[0]) {
            valid = copy_json_string(root, "mqtt_password", candidate.mqtt_password);
        }
    }
    if (root != nullptr) cJSON_Delete(root);
    candidate.provisioned = false;
    const auto validation = valid ? app::validate_config(candidate) : app::ConfigValidation::bad_header;
    if (!valid || validation != app::ConfigValidation::valid) {
        ESP_LOGW(tag, "Rejected setup save: %s",
                 app::config_validation_name(validation));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, app::config_validation_name(validation));
    }
    const esp_err_t config_save_result = app::ConfigStore{}.save(candidate);
    const esp_err_t monitor_save_result = config_save_result == ESP_OK
                                              ? app::save_monitor_output(
                                                    monitor_output_enabled)
                                              : ESP_ERR_INVALID_STATE;
    const esp_err_t pending_save_result =
        config_save_result == ESP_OK && monitor_save_result == ESP_OK
            ? app::ConfigStore{}.save_verification_pending(true)
            : ESP_ERR_INVALID_STATE;
    if (config_save_result != ESP_OK || monitor_save_result != ESP_OK ||
        pending_save_result != ESP_OK) {
        ESP_LOGE(tag, "Setup save failed: configuration=%s monitor=%s verification=%s",
                 esp_err_to_name(config_save_result),
                 esp_err_to_name(monitor_save_result),
                 esp_err_to_name(pending_save_result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save configuration");
    }
    self->config_ = candidate;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"saved\":true,\"restart\":true}");
    xTaskCreate(reboot_task, "config_reboot", 2048, nullptr, 2, nullptr);
    return ESP_OK;
}

esp_err_t WifiProvisioning::wifi_scan_handler(httpd_req_t *request)
{
    auto *self = static_cast<WifiProvisioning *>(request->user_ctx);
    if (self == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Provisioning state is unavailable");
    }
    if (self->scan_active_.exchange(true)) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "A Wi-Fi scan is already running");
    }

    bool reconnect_after_scan = false;
    const auto finish_scan = [&]() {
        self->scan_active_.store(false);
        if (reconnect_after_scan) {
            self->suppress_station_reconnect_.store(false);
            const esp_err_t connect_result = esp_wifi_connect();
            if (connect_result != ESP_OK) {
                ESP_LOGW(tag, "Could not resume station connection after scan: %s",
                         esp_err_to_name(connect_result));
            }
        }
    };
    const auto send_scan_error = [&](const char *status, const char *message) {
        finish_scan();
        httpd_resp_set_status(request, status);
        return httpd_resp_sendstr(request, message);
    };

    // ESP-IDF does not permit a scan while the station is in its connecting
    // state. Temporarily stop that attempt; the AP and setup page remain up.
    if (self->config_.wifi_ssid[0] != '\0' &&
        !self->station_connected_.load()) {
        self->suppress_station_reconnect_.store(true);
        reconnect_after_scan = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    wifi_scan_config_t scan{};
    scan.show_hidden = false;
    esp_err_t result = ESP_ERR_WIFI_STATE;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        result = esp_wifi_scan_start(&scan, true);
        if (result != ESP_ERR_WIFI_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (result != ESP_OK) {
        ESP_LOGW(tag, "Wi-Fi scan could not start: %s", esp_err_to_name(result));
        if (result == ESP_ERR_WIFI_STATE) {
            return send_scan_error("503 Service Unavailable",
                                   "Wi-Fi is busy connecting. Try Scan again.");
        }
        return send_scan_error("500 Internal Server Error",
                               "Could not start the Wi-Fi scan");
    }
    std::uint16_t available = 0;
    result = esp_wifi_scan_get_ap_num(&available);
    if (result != ESP_OK) {
        ESP_LOGW(tag, "Could not read Wi-Fi scan count: %s",
                 esp_err_to_name(result));
        return send_scan_error("500 Internal Server Error",
                               "Could not read the Wi-Fi scan results");
    }
    std::uint16_t count = std::min<std::uint16_t>(
        available, app::setup_wifi_scan_max_results);
    auto records = std::make_unique<wifi_ap_record_t[]>(count == 0 ? 1 : count);
    if (count != 0) {
        result = esp_wifi_scan_get_ap_records(&count, records.get());
        if (result != ESP_OK) {
            ESP_LOGW(tag, "Could not read Wi-Fi scan records: %s",
                     esp_err_to_name(result));
            return send_scan_error("500 Internal Server Error",
                                   "Could not read the Wi-Fi scan results");
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (std::uint16_t index = 0; index < count; ++index) {
        const char *ssid = reinterpret_cast<const char *>(records[index].ssid);
        if (ssid[0] == '\0') continue;
        bool duplicate = false;
        for (std::uint16_t previous = 0; previous < index; ++previous) {
            if (std::strcmp(ssid,
                            reinterpret_cast<const char *>(records[previous].ssid)) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", ssid);
        cJSON_AddNumberToObject(network, "rssi", records[index].rssi);
        cJSON_AddNumberToObject(network, "channel", records[index].primary);
        cJSON_AddBoolToObject(network, "secure",
                              records[index].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }
    finish_scan();
    send_json(request, root);
    return ESP_OK;
}

esp_err_t WifiProvisioning::finalize_handler(httpd_req_t *request)
{
    auto *self = static_cast<WifiProvisioning *>(request->user_ctx);
    const auto expected = self->config_.profile.expected_member_count;
    const auto found = std::count_if(
        self->config_.peers.begin(), self->config_.peers.end(),
        [](const app::SavedPeer &peer) {
            return peer.valid && peer.logical_member_id != 0 &&
                   peer.logical_member_id != 0xff;
        });
    if (app::role_is_espnow_coordinator(self->config_.profile.device_role) &&
        found < expected) {
        char error[96]{};
        std::snprintf(error, sizeof(error), "Waiting for ESP-NOW members: %u/%u", static_cast<unsigned>(found), expected);
        return send_conflict(request, error);
    }
    if (app::role_is_espnow_member(self->config_.profile.device_role) &&
        (self->mesh_ == nullptr || !self->mesh_->has_coordinator())) {
        return send_conflict(request, "Waiting for ESP-NOW coordinator");
    }
    if (app::role_uses_coordinator_link(self->config_.profile.device_role) &&
        (self->coordinator_link_ == nullptr || !self->coordinator_link_->connected())) {
        return send_conflict(request, "Waiting for coordinator-gateway UART link");
    }
    if (app::role_uses_mqtt(self->config_.profile.device_role) &&
        !self->station_connected_.load()) {
        return send_conflict(request, "Wi-Fi is not connected");
    }
    if (app::role_uses_mqtt(self->config_.profile.device_role) &&
        (self->mqtt_ready_probe_ == nullptr ||
         !self->mqtt_ready_probe_(self->mqtt_ready_context_))) {
        return send_conflict(request, "MQTT is not connected");
    }
    if (app::role_has_local_inverter(self->config_.profile.device_role) &&
        (self->inverter_ready_probe_ == nullptr ||
         !self->inverter_ready_probe_(self->inverter_ready_context_))) {
        return send_conflict(request, "Local inverter Modbus has not responded");
    }
    const esp_err_t result = self->complete_provisioning();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "NVS save failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"verified\":true,\"restart\":true}");
    xTaskCreate(reboot_task, "final_reboot", 2048, nullptr, 2, nullptr);
    return ESP_OK;
}

bool WifiProvisioning::required_connections_ready() const
{
    if (app::validate_config(config_) != app::ConfigValidation::valid) {
        return false;
    }
    const auto role = config_.profile.device_role;
    if (app::role_uses_mqtt(role) &&
        (!station_connected_.load() || mqtt_ready_probe_ == nullptr ||
         !mqtt_ready_probe_(mqtt_ready_context_))) {
        return false;
    }
    if (app::role_has_local_inverter(role) &&
        (inverter_ready_probe_ == nullptr ||
         !inverter_ready_probe_(inverter_ready_context_))) {
        return false;
    }
    if (app::role_is_espnow_member(role) &&
        (mesh_ == nullptr || !mesh_->has_coordinator())) {
        return false;
    }
    if (app::role_is_espnow_coordinator(role)) {
        const auto found = std::count_if(
            config_.peers.begin(), config_.peers.end(),
            [](const app::SavedPeer &peer) {
                return peer.valid && peer.logical_member_id != 0 &&
                       peer.logical_member_id != 0xff;
            });
        if (found < config_.profile.expected_member_count) return false;
    }
    return !app::role_uses_coordinator_link(role) ||
           (coordinator_link_ != nullptr && coordinator_link_->connected());
}

esp_err_t WifiProvisioning::complete_provisioning()
{
    if (completion_started_.exchange(true)) return ESP_ERR_INVALID_STATE;
    config_.provisioned = true;
    const esp_err_t config_result = app::ConfigStore{}.save(config_);
    if (config_result != ESP_OK) {
        config_.provisioned = false;
        completion_started_.store(false);
        return config_result;
    }
    const esp_err_t pending_result =
        app::ConfigStore{}.save_verification_pending(false);
    if (pending_result != ESP_OK) {
        ESP_LOGW(tag, "Could not clear setup-verification state: %s",
                 esp_err_to_name(pending_result));
    }
    return ESP_OK;
}

void WifiProvisioning::verification_task(void *argument)
{
    auto *self = static_cast<WifiProvisioning *>(argument);
    TickType_t ready_since = 0;
    while (self != nullptr && !self->completion_started_.load()) {
        if (self->required_connections_ready()) {
            if (ready_since == 0) ready_since = xTaskGetTickCount();
            if (xTaskGetTickCount() - ready_since >=
                pdMS_TO_TICKS(app::setup_verification_stable_ms)) {
                const esp_err_t result = self->complete_provisioning();
                if (result == ESP_OK) {
                    ESP_LOGI(tag,
                             "Setup verified; disabling setup AP and restarting in normal mode");
                    vTaskDelay(pdMS_TO_TICKS(750));
                    esp_restart();
                }
                ESP_LOGE(tag, "Automatic setup completion failed: %s",
                         esp_err_to_name(result));
                ready_since = 0;
            }
        } else {
            ready_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(app::setup_verification_check_ms));
    }
    if (self != nullptr) self->verification_task_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t WifiProvisioning::status_handler(httpd_req_t *request)
{
    auto *self = static_cast<WifiProvisioning *>(request->user_ctx);
    const auto found = std::count_if(self->config_.peers.begin(), self->config_.peers.end(),
                                     [](const app::SavedPeer &peer) {
                                         return peer.valid && peer.logical_member_id != 0 &&
                                                peer.logical_member_id != 0xff;
                                     });
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "site_id", self->config_.site_id);
    cJSON_AddBoolToObject(root, "provisioned", self->config_.provisioned);
    cJSON_AddStringToObject(root, "validation", app::config_validation_name(app::validate_config(self->config_)));
    cJSON_AddNumberToObject(root, "members_found", found);
    cJSON_AddNumberToObject(root, "members_expected", self->config_.profile.expected_member_count);
    cJSON *peers = cJSON_AddArrayToObject(root, "espnow_members");
    for (const auto &peer : self->config_.peers) {
        if (!peer.valid || peer.logical_member_id == 0 || peer.logical_member_id == 0xff) continue;
        cJSON *item = cJSON_CreateObject();
        char mac[18]{};
        std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                      peer.mac[0], peer.mac[1], peer.mac[2], peer.mac[3],
                      peer.mac[4], peer.mac[5]);
        cJSON_AddNumberToObject(item, "member_id", peer.logical_member_id);
        cJSON_AddStringToObject(item, "mac", mac);
        cJSON_AddItemToArray(peers, item);
    }
    cJSON_AddBoolToObject(root, "station_connected",
                          self->station_connected_.load());
    cJSON_AddBoolToObject(root, "mqtt_connected",
                          self->mqtt_ready_probe_ != nullptr &&
                          self->mqtt_ready_probe_(self->mqtt_ready_context_));
    cJSON_AddBoolToObject(root, "coordinator_uart_connected",
                          self->coordinator_link_ != nullptr &&
                          self->coordinator_link_->connected());
    cJSON_AddBoolToObject(root, "espnow_coordinator_found",
                          self->mesh_ != nullptr && self->mesh_->has_coordinator());
    cJSON_AddBoolToObject(root, "inverter_responding",
                          self->inverter_ready_probe_ != nullptr &&
                          self->inverter_ready_probe_(self->inverter_ready_context_));
    send_json(request, root);
    return ESP_OK;
}

void WifiProvisioning::reboot_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

} // namespace inverter_gateway::network
