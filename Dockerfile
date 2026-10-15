# syntax=docker/dockerfile:1
# Imagen de Telegarrm (docs/DECISIONS.md, D-054). No lleva ningún secreto: las claves de Telegram y
# TMDB llegan al arrancar, como Docker secrets (ver deploy/docker/compose.yaml). Debian trixie, como
# la Raspberry Pi. Tres etapas: TDLib, Telegarrm (compilado y probado) y la imagen final, solo con lo
# necesario para funcionar.

ARG DEBIAN_IMAGE=debian:trixie-slim

# --- 1) TDLib ---------------------------------------------------------------------------------------
# Siempre la misma versión (1.8.67, la de la Pi): la sesión de Telegram de una instalación se puede
# llevar a otra. Cambiarla es una decisión (D-054).
FROM ${DEBIAN_IMAGE} AS tdlib
ARG TDLIB_COMMIT=42e6a5259551178d1dab54a22ad96d14bd906e20
# Compilar TDLib necesita ~1,5 GB de memoria por proceso: en una Pi con 8 GB, como mucho 3 a la vez
ARG JOBS=3
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git gperf zlib1g-dev libssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN git init -q /src/td \
 && git -C /src/td fetch -q --depth 1 https://github.com/tdlib/td.git "${TDLIB_COMMIT}" \
 && git -C /src/td checkout -q FETCH_HEAD
RUN cmake -S /src/td -B /src/td/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DTD_INSTALL_STATIC_LIBRARIES=OFF \
 && cmake --build /src/td/build --target tdjson -j"${JOBS}" \
 && cmake --install /src/td/build \
 && rm -rf /src/td

# --- 2) Telegarrm -----------------------------------------------------------------------------------
FROM ${DEBIAN_IMAGE} AS build
ARG JOBS=3
# 7zip-rar está en non-free (por la licencia del códec de RAR). ffmpeg y 7-Zip: los usan los tests.
RUN sed -i 's/^Components: main$/Components: main non-free/' /etc/apt/sources.list.d/debian.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake libsqlite3-dev zlib1g-dev libssl-dev \
                       ffmpeg 7zip 7zip-rar \
 && rm -rf /var/lib/apt/lists/*
COPY --from=tdlib /usr/local /usr/local
# Los tests comprueban permisos de archivos: como root siempre se podría escribir
RUN useradd --create-home builder
COPY --chown=builder . /src/telegarrm
USER builder
WORKDIR /src/telegarrm
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 && cmake --build build -j"${JOBS}" \
 && ctest --test-dir build --output-on-failure \
 && strip --strip-debug build/telegarrm

# --- 3) Imagen final --------------------------------------------------------------------------------
FROM ${DEBIAN_IMAGE}
LABEL org.opencontainers.image.title="Telegarrm" \
      org.opencontainers.image.description="Descargas de series y películas desde Telegram, al estilo de Sonarr y Radarr" \
      org.opencontainers.image.source="https://github.com/Placix5/Telegarrm"
RUN sed -i 's/^Components: main$/Components: main non-free/' /etc/apt/sources.list.d/debian.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates tzdata libssl3t64 libsqlite3-0 zlib1g \
                       ffmpeg 7zip 7zip-rar \
 && rm -rf /var/lib/apt/lists/*
COPY --from=tdlib /usr/local/lib/libtdjson.so* /usr/local/lib/
RUN ldconfig
WORKDIR /app
COPY --from=build /src/telegarrm/build/telegarrm /app/telegarrm
COPY web /app/web
# Datos (BD, sesión de Telegram, contraseña de la web): /app/db, montada desde el servidor.
# HOME en /tmp: ffmpeg y 7-Zip no tienen nada que guardar.
ENV TZ=Europe/Madrid HOME=/tmp
USER 1000:1000
EXPOSE 8080
ENTRYPOINT ["/app/telegarrm"]
