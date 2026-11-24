# WiFi and application session recovery

This guide describes how to restore WiFi connectivity and resume network-dependent services after an access point (AP) interruption, an IP configuration failure, or a stalled application session. Use the platform-independent design with your network stack. The ESP-IDF mapping below shows how to implement it for an IPv4 station.

## 1. Define startup and recovery behavior

On startup, load the saved network configuration and initiate a connection. If credentials are missing, enter the project's provisioning flow. Once configured, keep recovery active throughout normal operation, including when all network-dependent services are idle. During a temporary outage, retain the saved configuration and keep local functions available where possible.

## 2. Integrate the ESP-IDF WiFi provisioner

This setup uses `michmich/esp-idf-wifi-provisioner` from the ESP Component Registry. The integration below follows version 0.4.0. Add the dependency from the project root.

```sh
idf.py add-dependency "michmich/esp-idf-wifi-provisioner"
```

The provisioner loads credentials from NVS and attempts the initial station connection. If credentials are missing or the boot-time connection fails, it starts a captive portal. Its station connection handlers are scoped to the initial attempts. The application recovery manager takes over after successful setup.

1. **Initialize before subscribing.** Call `wifi_prov_init()` to initialize NVS, ESP-NETIF, and the default event loop. Coordinate this with application startup so those resources are initialized once. Create the recovery manager and register its persistent WiFi and IP handlers before calling `wifi_prov_start()`. Keep runtime reconnect actions disabled during provisioning.
2. **Start the provisioner.** Include `wifi_provisioner.h`, initialize a `wifi_prov_config_t` with `WIFI_PROV_DEFAULT_CONFIG()`, configure the setup AP, and assign `on_connected` and optionally `on_portal_start`. Call `wifi_prov_start(&config)` from startup or an initialization task and check the result. A successful return can mean the portal is running. Local application functions can start independently.
3. **Hand off through `on_connected`.** Have this callback post a message to the recovery manager. It runs after a successful connection from stored credentials or after successful portal setup. When the manager handles the message, enable runtime recovery and read the current association and IP state. The first `GOT_IP` may have arrived before handoff. If the link has since dropped, schedule recovery immediately. Start network services when their readiness requirements are met.
4. **Recover with the existing STA configuration.** On a runtime disconnect, let the manager schedule `esp_wifi_connect()` with backoff, using credentials already configured in the driver. Once IP is ready again, rebuild affected sessions. In version 0.4.0, `wifi_prov_is_connected()` records initial success and can remain true during a runtime outage. Use the manager's link and IP state for live status.
5. **Reserve provisioning for setup and reconfiguration.** Keep saved credentials during runtime outages. For an explicit network change, suspend recovery and cancel pending reconnects before entering the project's reprovisioning flow. Test boot with the AP unavailable separately, since exhausting the initial retries can open the portal.

[ESP Component Registry documentation](https://components.espressif.com/components/michmich/esp-idf-wifi-provisioner/versions/0.4.0/readme) · [Provisioner lifecycle source](https://github.com/MichMich/esp-idf-wifi-provisioner/blob/v0.4.0/src/wifi_provisioner.c) · [STA connection source](https://github.com/MichMich/esp-idf-wifi-provisioner/blob/v0.4.0/src/wifi_sta.c)

## 3. Implement three layers

1. **Connection manager.** Give one task or event loop ownership of the desired station state, the current connection attempt, and the retry timer. Let the provisioning layer load or configure credentials at startup. Handle runtime recovery signals as soon as the station becomes operational, including during a startup screen or while the application is idle. If a library already reconnects automatically, leave reconnect execution to that library and avoid duplicate calls.
2. **Session manager.** Give each long-lived connection one owner for its socket, protocol state, and start and stop operations. After network connectivity is lost, create a new transport and application session once IP is available again. Associate asynchronous results with a session ID so a late callback from an old session cannot modify the new one. Restore protocol state such as MQTT subscriptions or a resume cursor.
3. **Application monitoring.** Define what counts as observable progress for each service. For a live media stream, it is the delivery of usable frames. For MQTT, it might be a protocol heartbeat or an expected acknowledgment. For an upload, it could be a confirmed byte offset or a completed unit of work. Monitor progress only when it is expected. For a listening server with no clients, monitor link and IP events and the state of the listener.

## 4. Make recovery decisions based on link, IP, and progress

| Signal | Action |
|---|---|
| Station disconnected | Mark the network unavailable and invalidate existing client sessions. If WiFi is still wanted, schedule one reconnect attempt using the existing configuration. |
| Station associated, IP unavailable | Wait for IP assignment with a deadline. If the deadline expires repeatedly, investigate IP configuration or DHCP and start a new station connection attempt according to the retry policy. |
| IP available or changed | Start the required client sessions or rebind address-dependent services. Make these operations idempotent so duplicate events cannot create duplicate connections or listeners. |
| IP available, service making no progress | First ask the service owner to open a new session. Use DNS, TCP, TLS, or protocol errors to diagnose the issue. Limit recovery to that service while the network remains healthy. |
| The same service keeps failing | After a bounded number of complete attempts, check for a configured alternate source. Start it once the network is usable. If no alternate is available, keep the service in a degraded state and retry the same source according to its backoff policy. |
| Stop, provisioning, reconfiguration, or maintenance | Cancel pending attempts that no longer match the desired state. Suppress automatic reconnect after an intentional disconnect. |

For IPv4 on ESP-IDF, use `WIFI_EVENT_STA_CONNECTED`, `WIFI_EVENT_STA_DISCONNECTED`, `IP_EVENT_STA_GOT_IP`, and `IP_EVENT_STA_LOST_IP` to drive state transitions. Filter IP events for the intended interface. A periodic check can use `esp_wifi_sta_get_ap_info()` to confirm station association, `esp_netif_is_netif_up()` to check interface state, and `esp_netif_get_ip_info()` to check the address. With IPv6 or multiple interfaces, explicitly choose the route and address family each service needs. [ESP-NETIF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif_programming.html)

## 5. Implement the recovery cycle

1. **Keep callbacks short.** Copy each event and its relevant reason or network data to the manager. Use FreeRTOS queues, notifications, or an equivalent event loop. Have the manager serialize timers, retries, and session starts. If the event queue overflows, reconcile with the current state.
2. **Allow one in-flight attempt per layer.** Give association, IP assignment, and session establishment separate deadlines. Use a monotonic clock and make waits cancellable. Add jitter to retry backoff if several devices might reconnect after the same AP outage. Check the return code from a connect or start request before waiting for its result.
3. **Use the regular session startup path for recovery.** The session owner closes the previous session's resources and opens the new session through the same path used for the first connection. Recreate client sessions when the network address changes. For listeners, discovery, and local management services, determine which need to rebind or advertise again when IP returns.
4. **Confirm recovery at the service level.** Track an outage from the first missed progress signal until observable recovery or an explicit user action. Give each attempt time to reach that point. A response header can confirm the transport phase. The first usable frame, expected acknowledgment, or completed unit of work confirms the application phase.
5. **Size buffers for the workload.** Separate blocking I/O from time-critical processing. Choose buffer capacity and prefill based on the interruption you want to bridge, data rate, memory budget, and acceptable startup latency. For outgoing data, decide which messages can expire and which must be stored durably and retried safely.
6. **Measure recovery.** Log state transitions, disconnect reasons, retry phases, attempt numbers, session IDs, time to IP availability, and time to first progress. For streaming, minimum buffer fill and underrun counts are useful additional metrics. Keep credentials and tokens out of logs.

Set deadlines based on expected AP startup time, DHCP latency, transport timeouts, and the longest interruption the service can tolerate. A fixed limit can bound escalation for each service while connection retries continue with backoff.

## 6. Configure retry and escalation policy

Define separate time budgets for association, IP assignment, and application progress. Measure normal and degraded behavior on the target network before fixing the values.

| Setting | Selection criterion |
|---|---|
| Association and IP deadlines | Allow for typical association, authentication, and address assignment latency. Log each deadline separately so you can identify where the attempt stopped. |
| Connection retry backoff | Start with a short delay, increase it after successive failures up to a configured cap, and add jitter. Reset the backoff after the network has recovered according to your readiness criteria. |
| Application progress deadline | Set it from the expected period between frames, heartbeats, acknowledgments, or other meaningful progress. Account for normal idle periods, buffering, and startup latency. |
| Session retry budget | Count completed failed attempts. Preserve the count across session restarts within the same outage. Clear it after confirmed progress or an explicit change in the requested service. |
| Failover | Define which endpoints are interchangeable, how long to wait before switching, and when to retry the primary endpoint. |

If connection attempts repeatedly fail even though the AP and network are available, a platform-specific adapter restart can be a separate escalation step. Run it through the connection manager so it coordinates session shutdown and restarts the adapter with the saved configuration. Rate-limit that action and log when it occurs.

## 7. Verify on the target device

| Test | Expected result |
|---|---|
| Boot with valid stored credentials, then repeat with no credentials | Runtime recovery takes over after setup through either path. Each required service starts once. |
| Restart the AP during use and repeat with no active service | The station regains IP in both cases without intervention. An active service also resumes delivering usable data. |
| Keep the AP off for longer than one recovery cycle | Retries remain rate-limited. When the AP returns, the device recovers using stored credentials. |
| Let association succeed and delay DHCP | Session startup waits for IP. The deadline expires cleanly and a subsequent attempt can succeed. |
| Block only the service or stop payload delivery while leaving TCP open | The progress deadline triggers session recovery. The station remains connected and other local functions stay reachable. |
| Switch sources or begin maintenance during a retry | The pending attempt is canceled. Late results are ignored. |
| Make every attempt to the primary source fail | An available fallback is tried according to policy. Without a fallback, status and retry behavior remain predictable. |
| Repeat the outage and recovery many times | Socket counts, task counts, and memory use remain stable. Measure time to IP and time to a working service across the entire series. |
