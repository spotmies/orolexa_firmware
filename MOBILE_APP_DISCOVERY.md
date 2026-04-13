# Discovering the DentalS3 camera from your React Native app

The ESP32 firmware advertises the camera stream over **mDNS** (Bonjour/Zeroconf). Your React Native app can discover the device’s **IP address and port** on the local network without the user typing anything.

## 1. ESP32 side (already done)

When the device connects to Wi‑Fi it:

- Sets mDNS hostname: **`dentals3`** (reachable as `dentals3.local`)
- Advertises service: **`_http._tcp`** on port **81**
- Instance name: **`DentalS3 Camera`**
- TXT record: **`path=/stream`**

So the stream URL is: `http://<IP>:81/stream` (or `http://dentals3.local:81/stream` if the OS resolves `.local`).

---

## 2. React Native: use Zeroconf (mDNS) discovery

Install [react-native-zeroconf](https://github.com/balthazar/react-native-zeroconf):

```bash
npm install react-native-zeroconf
# or
yarn add react-native-zeroconf
```

Then link (if not auto-linked):

```bash
cd ios && pod install && cd ..
```

### Permissions

- **Android** (`AndroidManifest.xml`): `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_MULTICAST_STATE`
- **iOS** (`Info.plist`):
  - Add to **NSBonjourServices**: `_http._tcp`
  - Add **NSLocalNetworkUsageDescription** (e.g. “Used to find the DentalS3 camera on your Wi‑Fi”).

### Example: discover camera and get stream URL

```javascript
import Zeroconf from 'react-native-zeroconf';

const zeroconf = new Zeroconf();

// Optional: restrict to your device by name
const DENTALS3_INSTANCE = 'DentalS3 Camera';
const SERVICE_TYPE = 'http';
const SERVICE_PROTO = 'tcp';

zeroconf.on('resolved', (service) => {
  // Filter to our camera (optional; remove to list all _http._tcp devices)
  if (service.name !== DENTALS3_INSTANCE) return;

  const ip = service.addresses && service.addresses[0]; // IPv4
  const port = service.port;
  if (ip && port) {
    const streamUrl = `http://${ip}:${port}/stream`;
    const baseUrl = `http://${ip}:${port}`;
    console.log('Camera found:', streamUrl);
    // e.g. set state: setCameraBaseUrl(baseUrl); setStreamUrl(streamUrl);
  }
});

zeroconf.on('error', (err) => console.warn('Zeroconf error', err));

// Start discovery (same Wi‑Fi as the ESP32)
zeroconf.scan(SERVICE_TYPE, SERVICE_PROTO, 'local.');

// When leaving the screen or closing the flow:
// zeroconf.stop();
```

- **`service.addresses`**: array of IP strings (typically IPv4 first).
- **`service.port`**: port (81 for this firmware).
- **`service.name`**: instance name (“DentalS3 Camera”).

You can build the stream URL as `http://${ip}:${port}/stream` and the API base as `http://${ip}:${port}` (e.g. for `GET /api/capture_request`).

### Optional: use hostname instead of IP

If your React Native stack resolves mDNS hostnames (e.g. via a native module or a library that uses the system resolver), you can use:

- **Stream:** `http://dentals3.local:81/stream`
- **Base:** `http://dentals3.local:81`

Otherwise, using the **IP** from `service.addresses[0]` and `service.port` is the most reliable.

---

## 3. Summary

| What        | Value              |
|------------|--------------------|
| mDNS host  | `dentals3.local`   |
| Service    | `_http._tcp`       |
| Port       | 81                 |
| Instance   | DentalS3 Camera    |
| Stream URL | `http://<IP>:81/stream` |

The app only needs to run Zeroconf discovery on the same Wi‑Fi; no serial monitor or manual IP entry is required.
