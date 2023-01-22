FROM golang:1.18-alpine

WORKDIR /app

COPY go.mod ./
COPY go.sum ./

RUN go mod download

COPY internal/ internal/
COPY *.go ./

RUN go build -o osm-cache main.go

EXPOSE 8000

CMD [ "./osm-cache" ]