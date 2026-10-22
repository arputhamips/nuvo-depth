FROM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
WORKDIR /workspace
COPY tools/dev/bootstrap-host.sh /tmp/bootstrap-host.sh
RUN bash /tmp/bootstrap-host.sh --full
CMD ["bash"]
