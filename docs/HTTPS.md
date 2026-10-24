# HTTPS and certificates

How to open the web app over HTTPS, get rid of the browser's warning with a certificate of your own or one from your
ACME server, and use the trust store for MQTT over TLS. Firmware 2.4.1 and later, with the `tls` package (in the
prebuilt firmware and the examples); ACME needs the `acme` package (in the prebuilt firmware), MQTT over TLS the
`mqtt_tls` package (in the prebuilt firmware). How it works and why: [DESIGN.md](../DESIGN.md) section 9.7.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes. Tested on a bench feeder with
firmware 2.4.4 (docs/BENCH.md); what is still untested is in DESIGN.md section 14, item 27.*

## Opening the app over HTTPS

Browse to `https://<name>.local` (the prebuilt firmware: `https://precision-feeder-a1b2c3.local`, with the end of
your board's MAC address), or `https://` and its IP address. Opening `http://<name>.local` sends you there by itself
(**Send port 80 to HTTPS**, on by default), and so does Home Assistant's "Visit" link.

The first time, your browser warns that the connection is not private. That is expected: out of the box the feeder
uses a certificate it made itself, which no browser knows. Check that the address is your feeder's, then accept it
(Chrome and Edge: Advanced, Proceed; Firefox: Advanced, Accept the Risk; Safari: Show Details, visit this website).
Your browser remembers it for that feeder. It warns again when the feeder makes a new certificate: after you change
its name, or ask for a new one.

To have no warning at all, give the feeder a certificate your devices already trust: upload one from your own CA, or
let it get one from your ACME server. Both are in the web app, **Configuration, HTTPS and certificates**, and need
the settings password if you set one.

## The certificate in use

The card shows where the certificate comes from, the name in it, when it expires, its SHA-256 fingerprint (to
compare with what your browser shows), how many HTTPS connections are open, and, when an uploaded certificate is not
used, why. The feeder picks, in this order:

1. **From ACME**, while ACME is set up and its certificate is valid.
2. **Uploaded**, while it still checks (below).
3. **Self-signed**, made on the feeder at its first start. Always there, so HTTPS never stops working because of a
   certificate.

## Your own certificate

For a CA you already run (a home lab CA, step-ca without ACME, a company CA).

What the feeder takes:
- the certificate first, then any intermediates, as PEM (`-----BEGIN CERTIFICATE-----`), at most 6 KB in all;
- its private key as PEM, **not encrypted**: an EC P-256 key (recommended: smaller and faster) or RSA 2048;
- the feeder's name in the certificate's Subject Alternative Names (`DNS:<name>.local`, and the IP address if you
  open it by address). Browsers ignore the Common Name.

Example with OpenSSL, for a CA whose files are `ca.crt` and `ca.key`:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out feeder.key
openssl req -new -key feeder.key -subj "/CN=precision-feeder-a1b2c3.local" -out feeder.csr
openssl x509 -req -in feeder.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -sha256 \
  -extfile <(printf "subjectAltName=DNS:precision-feeder-a1b2c3.local\nextendedKeyUsage=serverAuth") -out feeder.crt
```

Then **Upload a certificate**: paste the certificate (and chain) and the key, or pick the files (one file holding
both, in either box, works too). The page refuses an encrypted key and anything too large before sending. The key travels masked
with the settings password; with no password set it crosses the network as it is, so set one first. The feeder
checks the upload again: every certificate must parse, the key must match the first one, and, when the trust store
holds a CA, the chain must verify against it; its dates are checked once the clock is set. A certificate that passes
is used **after a restart** (Restart Needed); one that stops passing later (it expired, or you changed the trust
store) gives way to the self-signed one at the next start, and the card says why.

**Remove it** goes back, after a restart, to the ACME certificate if there is one, else the self-signed one. With
the self-signed one in use, **Make a new one** makes a new one at the next start (your browser warns once more).

## A certificate from your ACME server

For an ACME server on your network, for example [step-ca](https://smallstep.com/docs/step-ca/). The feeder makes its
own keys (they never leave it), proves it owns its name with the http-01 challenge on port 80, and renews the
certificate by itself, two thirds into its lifetime. Public ACME services such as Let's Encrypt cannot reach a feeder
on your LAN or issue for a `.local` name, so they do not work here.

What the ACME server needs:
- an ACME provisioner (step-ca: `step ca provisioner add acme --type ACME`, then restart step-ca);
- to resolve the feeder's name (`<name>.local` is mDNS: the CA's host must run an mDNS resolver, such as Avahi on
  Linux), and to reach its port 80 (a firewall between your IoT network and the CA must let it through);
- if it has a name policy, to allow `*.local`.

On the feeder:
1. **Trust store:** upload your CA's root certificate (step-ca: `$(step path)/certs/root_ca.crt`), then restart. The
   feeder checks the ACME server against it, and without it ACME does not run: it never talks to a server it has not
   checked.
2. **ACME directory:** the provisioner's directory URL, `https://` only (step-ca:
   `https://<ca host>:<port>/acme/<provisioner name>/directory`, for example
   `https://ca.lan:9000/acme/acme/directory`).
3. **ACME name:** leave it empty for `<name>.local`, or set the name the certificate should carry.

ACME runs once the clock is set. The card's ACME line says what it did last; a first certificate takes about half a
minute. It is served at once, without a restart: new connections get it, open ones keep theirs. **Renew now** asks
again straight away.

If a renewal fails, the feeder keeps the certificate it has and tries again after 5 minutes, 15 minutes, an hour,
then every 3 hours. Once that certificate has expired, the feeder serves the uploaded one, or the self-signed one,
until ACME succeeds again; it never restarts for it. Clearing the directory goes back the same way. step-ca issues
certificates for 24 hours by default, so the feeder renews about every 16 hours.

## Port 80 and HTTPS only

The feeder keeps answering on port 80: phones look for the captive portal there, the ACME challenge is answered
there, and Home Assistant's link points there.

- **Send port 80 to HTTPS** (on by default): opening the app on port 80 sends the browser to HTTPS. Everything else
  on port 80 still works in plain HTTP.
- **HTTPS only** (off by default): port 80 answers only that redirect and the ACME challenge; everything else is
  refused. It never applies while the feeder's access point is up (the setup wizard and the captive portal stay
  plain HTTP), nor until HTTPS is running after a start, so a failure cannot lock you out.

Turn on HTTPS only once you open the app over HTTPS without trouble. Scripts that call the feeder's web server must
then use HTTPS too.

## The trust store

One slot for your own CA: one or more PEM certificates (a root, or a root and an intermediate), at most 6 KB.
Upload, replace or clear it in the same card; a change is used after a restart. It is what the feeder trusts as a
client:

- **MQTT over TLS** checks the broker against it (with "CA certificate"; [MQTT.md](MQTT.md)).
- **ACME** checks the ACME server against it.
- An **uploaded certificate** must verify against it, when it holds one.

There is no list of public CAs on the feeder: a broker or ACME server with a public certificate needs its root
uploaded here like any other.

## MQTT over TLS

In Configuration, MQTT: turn on **TLS**, set the broker's TLS port (usually 8883), and choose **Check the broker**
(MQTT TLS and MQTT TLS Check in Diagnostics):

- **CA certificate:** the broker is checked against the trust store, its name included. Use this when you can.
- **Pin on first contact:** the feeder keeps the certificate the broker shows the first time (a self-signed one, or
  the CA at the top of its chain) and accepts only that from then on, like SSH. Safe as long as nobody impersonates
  the broker on that first connection. A broker that sends only its own certificate, signed by a CA it does not
  send, cannot be pinned: upload the CA instead. **Forget** pins again at the next start, after the broker's
  certificate changes.

These apply after a restart. With TLS on, MQTT never falls back to plain text: until it can check the broker, it
does not connect. Details: [MQTT.md](MQTT.md).

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| The browser still warns with my certificate | The certificate is not in use yet (restart after the upload), the name you opened is not in its Subject Alternative Names, or the device you browse from does not trust your CA (install the root there). Compare the fingerprint in the card with the browser's. |
| "The uploaded certificate is not used" | It no longer checks: it expired, or the trust store now holds a CA it does not chain to. Upload a new one, or fix the trust store, then restart. |
| The upload is refused | The key does not match the certificate, the key is encrypted, the files are too large, or the chain does not verify against the trust store. |
| ACME says it has no trust store, or "its certificate does not check against the trust store" | Upload your CA's root to the trust store (the root, not the intermediate). |
| ACME says "not reachable" or "name not found" | The feeder cannot reach the directory URL: check the host name, the port, and a firewall between the feeder and the CA. |
| ACME fails at the challenge | The ACME server cannot resolve `<name>.local` or reach the feeder's port 80: check mDNS on the CA's host and the firewall. Make sure the ACME name is the one the CA can resolve. |
| ACME does nothing | It waits for the clock (Diagnostics, Clock), and for a directory URL. |
| MQTT does not connect with TLS on | The trust store has no CA (with "CA certificate"), the broker's name is not in its certificate, the port is the plain one, or the pin cannot be made: the MQTT TLS status says which. Settings apply after a restart. |
| Pages are slow to open over HTTPS | A new connection takes about half a second to set up; at most three are open at once, and a fourth waits. A page in the background lets go of its connection after 30 seconds. |
| Locked out with HTTPS only on | Open `https://` instead of `http://`. If HTTPS itself fails, port 80 opens again for the first seconds after a start and whenever HTTPS is not running; the access point (Forget network, or out of Wi-Fi range) also brings plain HTTP back. |

## Limits

- **The keys are in flash in the clear** (no flash encryption): anyone holding the board can read the HTTPS key. See
  [SECURITY.md](SECURITY.md).
- **TLS 1.2 only**, with ECDHE and AES-GCM: every current browser supports it.
- **At most three HTTPS connections at once** (a YAML build can set 1 to 6, `max_sessions`); each takes about 20 KB of
  memory while open.
- **http-01 only.** No DNS challenge (it would keep DNS credentials on the feeder), no wildcard names.
- **No client certificates** for MQTT (mutual TLS).
