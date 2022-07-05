docker build -t osm-cache .
docker stop osm-cache
docker container rm osm-cache
docker run --name osm-cache -d -p 8000:8000 -v tiles:/app/tiles osm-cache