# The tunnel: a tiny image with autossh and nothing else. Built locally
# (docker compose build) rather than pulled from a third-party autossh image,
# so the only code that runs is Alpine's own packages.
FROM alpine:3.20
RUN apk add --no-cache autossh openssh-client
# Not root: the container only needs to make one outgoing SSH connection.
RUN adduser -D -H tunnel
USER tunnel
COPY --chmod=755 tunnel.sh /usr/local/bin/tunnel.sh
ENTRYPOINT ["/usr/local/bin/tunnel.sh"]
