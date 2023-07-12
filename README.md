# Osmium

Osmium a small proxy server written in Go capable to cache OpenStreetMap 
tiles in the disk and serve them without the need to reach OSM servers.

## Build instructions
### With Make

You can use Make to compile. Just use one of the following commands to compile to your target OS:

- ```make build-linux```
- ```make build-windows```
- ```make build-darwin``` (MacOS)

### With Docker

There's a Dockerfile in the project root. You can create a container using 
```./scripts/create-container.sh```.

## Roadmap

This is a very simple project and it might be improved.

- Write unit tests and improve the code testability;
- Create an option to store tiles in AWS S3 (or other cloud storages);
- An endpoint to find a tile by its WGS84 coordinate;