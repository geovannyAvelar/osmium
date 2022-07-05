# OpenStreetMap Tiles Cache Server

This repository contains a small proxy server written in Go 1.18 capable to cache OpenStreetMap 
tiles in the disk and serve them wtihout the need to reach OSM servers.

## Build instructions

This code uses Gorilla Mux to generate HTTP routes, all dependencies are included in the go.mod 
file.

You can use Make to compile. Just use one of the following commands to compile to your target OS:

- ```make build-linux```
- ```make build-windows```
- ```make build-darwin``` (MacOS)

## Roadmap

This is a very simple project and it might be improved.

- Write unit tests and improve the code testability;
- Create an option to store tiles in AWS S3 (or other cloud storages);
- Allow users to choose other map providers, not just OSM;
- An endpoint to find a tile by its WGS84 coordinate.