# Remote access to the weigh station

View the station's web app from outside the makerspace, without opening any
port on the makerspace network and without changing the firmware.

```
browser ──HTTPS + login──► public server (nginx) ◄──SSH tunnel── Docker host at the space ──LAN──► weigh station
```

The Docker host makes **one outgoing SSH connection** to the public server and
publishes the station on that server's loopback interface. nginx on the
public server adds HTTPS and a login, then forwards to it. Nothing at the
makerspace accepts an incoming connection.

## Why it is built this way

The station's web server assumes the lab LAN is its security boundary: no
HTTPS, reads always open, writes open unless an API key is set, and room for
only a few connections at once. So it must never be reachable directly from
the internet — no port forward on the makerspace router. Everything that
makes remote access safe lives on the public server.

An SSH reverse tunnel rather than a VPN because the Docker host here is
someone else's machine: the container needs no extra privileges, no host
networking and no kernel modules, only outgoing SSH.

## 0. On the station

1. **Give it a fixed address** — a DHCP reservation on the makerspace router.
   `weighstation.local` is mDNS and does not resolve from inside a container.
2. **Set an API key** (Settings page, or `APIKEY <secret>` over serial). The
   login on nginx guards everything, but the key makes writes need a second
   secret, and scripts using the remote URL must send it as `X-API-Key`.

## 1. On the public server (once)

1. **A tunnel-only user.** It can open the one forward and nothing else:

   ```sh
   sudo adduser --system --group --shell /usr/sbin/nologin --home /var/lib/weighstation-tunnel weighstation-tunnel
   sudo usermod -p '*' weighstation-tunnel
   sudo install -d -m 700 -o weighstation-tunnel -g weighstation-tunnel /var/lib/weighstation-tunnel/.ssh
   ```

   The `usermod` matters: `adduser --system` creates a **locked** account
   (`!`), and sshd refuses locked accounts outright when PAM is off — the key
   is then rejected with a bare "Permission denied". `*` means "no password"
   instead: still no password login, but key login works.

2. **Its authorized key**, in `/var/lib/weighstation-tunnel/.ssh/authorized_keys`,
   mode 600, owned by that user. The public key comes from step 2.2 below:

   ```
   restrict,port-forwarding,permitlisten="127.0.0.1:8081",command="/bin/false" ssh-ed25519 AAAA... weighstation-tunnel
   ```

3. **What that user may do, in `/etc/ssh/sshd_config`** (at the end — a
   `Match` block runs to the end of the file):

   ```
   Match User weighstation-tunnel
       AllowTcpForwarding remote
       PermitListen 127.0.0.1:8081
       GatewayPorts no
       PermitTTY no
       X11Forwarding no
       AllowAgentForwarding no
       ForceCommand /bin/false
   ```

   then `sudo sshd -t && sudo systemctl reload ssh`. The key line alone is not
   enough: `port-forwarding` re-enables *local* forwards too, which would let
   whoever holds the key reach anything this server can reach. `AllowTcpForwarding
   remote` is what blocks that (`permitopen="none"` in the key line looks like the
   answer but makes sshd reject the key altogether). Both layers together allow
   exactly one thing: listening on `127.0.0.1:8081`. Needs OpenSSH 7.8 or later.

4. **The nginx site.** Copy `nginx-weighstation.conf` to
   `/etc/nginx/sites-available/weighstation`, replace `weighstation.example.com`
   with the real name, enable it, and get a certificate
   (`sudo certbot --nginx -d <name>`).

5. **The login:**

   ```sh
   sudo htpasswd -c /etc/nginx/weighstation.htpasswd <username>
   sudo nginx -t && sudo systemctl reload nginx
   ```

## 2. On the makerspace Docker host (once)

1. Copy this directory there. Then `cp .env.example .env` and fill it in.
2. **A key for the tunnel, made for this purpose only:**

   ```sh
   mkdir -p keys
   ssh-keygen -t ed25519 -N '' -C weighstation-tunnel -f keys/id_ed25519
   cat keys/id_ed25519.pub     # -> the public server's authorized_keys (step 1.2)
   ```

3. **Pin the public server's host key**, so the tunnel cannot be pointed at an
   impostor:

   ```sh
   ssh-keyscan -p 22 sohl.example.com > keys/known_hosts
   ```

   Check the fingerprint against the server's own
   (`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` there) before trusting it.

4. **Start it:**

   ```sh
   docker compose up -d --build
   docker compose logs -f      # quiet when it is working
   ```

   `restart: unless-stopped` brings it back after a reboot of the host, and
   autossh reconnects if the connection drops. Needs BuildKit (the default
   in current Docker) for `COPY --chmod`.

## Checking it

- On the public server: `curl -s http://127.0.0.1:8081/api/status` returns
  the station's status JSON. If not, the tunnel is down — check
  `docker compose logs` on the Docker host.
- In a browser: `https://<name>/` asks for the login, then shows Inventory.
  Place a spool at the station and the page should update by itself; if it
  does not, `/events` is being buffered (see the nginx file).

## What has been tested

Tested 2026-10-01 with OpenSSH 9.6, autossh 1.4g and nginx 1.24, end to end
through this exact `tunnel.sh`, sshd setup and nginx site, with a stand-in
web server for the station that streams `/events` like the real one (Docker
was not available there, so the image was not built; `tunnel.sh` ran
directly):

- the tunnel serves the station's pages on the public server's loopback;
- autossh reconnects by itself after the public server's sshd restarts;
- with the key: running a command, listening on any other port, listening on
  a non-loopback address, and local forwards (`-L`) are all refused;
- nginx: plain HTTP redirects to HTTPS; no login or a wrong one gets 401 on
  every path, `/events` included; with the login, pages come through;
- `/events` streams live through nginx (4 events in 3.5 s). With
  `proxy_buffering` switched on instead, none arrived in the same time —
  which is exactly the failure that setting exists to prevent.

Not yet tested: the Docker image build, a real certbot certificate, and the
real station behind it.

## Keep in mind

- **One or two remote viewers.** The station serves only a few connections at
  once, and each open page holds one for live updates. Lab users come first.
  Dashboards should poll `/api/*` gently.
- **When the tunnel or station is down**, nginx answers 502. Nothing is lost:
  the station records locally whether or not anyone is watching.
- **To revoke access**, delete the line from `authorized_keys` on the public
  server; the container stops being able to connect.
- `keys/` and `.env` hold secrets and addresses: they are git-ignored.
