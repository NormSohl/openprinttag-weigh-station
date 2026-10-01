#!/bin/sh
# Hold one outgoing SSH connection to the public server and publish the
# weigh station's web app on that server's loopback interface:
#
#   public server 127.0.0.1:$REMOTE_PORT  ->  this container  ->  $SCALE_IP:80
#
# autossh restarts the connection if it drops; ServerAlive* is what makes a
# dead connection detectable in the first place (-M 0 relies on it).
set -eu

: "${SCALE_IP:?set SCALE_IP to the weigh station's fixed LAN address}"
: "${SOHL_HOST:?set SOHL_HOST to the public server's hostname}"
: "${SOHL_USER:=weighstation-tunnel}"
: "${SOHL_SSH_PORT:=22}"
: "${REMOTE_PORT:=8081}"

# ssh refuses a private key that others can read. The key arrives through a
# read-only volume owned by whoever is on the host, so copy it somewhere this
# user owns and tighten it, rather than depending on the host's permissions.
mkdir -p /tmp/ssh
cp /keys/id_ed25519 /tmp/ssh/id_ed25519
chmod 600 /tmp/ssh/id_ed25519

exec autossh -M 0 -N \
    -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
    -o ExitOnForwardFailure=yes \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/keys/known_hosts \
    -i /tmp/ssh/id_ed25519 -p "$SOHL_SSH_PORT" \
    -R "127.0.0.1:${REMOTE_PORT}:${SCALE_IP}:80" \
    "${SOHL_USER}@${SOHL_HOST}"
