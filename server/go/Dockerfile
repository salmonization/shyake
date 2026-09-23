# shyake-server in a container: a static binary on distroless, running
# as a non-root user, with its database on the /data volume.
#
#   docker build --build-arg VERSION=$(cat server/VERSION) \
#       -t shyake-server server/go
#   docker run -d -p 127.0.0.1:8787:8787 -v shyake:/data \
#       -e SHYAKE_INSTANCE_DOMAIN=mail.example.org shyake-server

FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
ARG VERSION=dev
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" \
        -o /out/shyake-server ./cmd/shyake-server \
    && mkdir -p /out/data

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/shyake-server /usr/local/bin/shyake-server
# a new named volume takes this ownership, so the non-root user can write
COPY --from=build --chown=65532:65532 /out/data /data
ENV SHYAKE_LISTEN=0.0.0.0:8787 \
    SHYAKE_DATABASE=/data/shyake.db
VOLUME /data
EXPOSE 8787
ENTRYPOINT ["/usr/local/bin/shyake-server"]
