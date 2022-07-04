package main

import (
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"runtime"
	"strconv"

	"github.com/apeyroux/gosm"
	"github.com/gorilla/mux"
)

const OSM_TILES_URI = "https://tile.openstreetmap.org/%d/%d/%d.png"

var errDownloadTilesLimit = errors.New("Cannot download more than 250 tiles in zoom levels higher than 13")

func newVectorFromMapParams(params map[string]string) (*gosm.Tile, error) {
	xParam, isXPresent := params["x"]
	yParam, isYPresent := params["y"]
	zParam, isZPresent := params["z"]

	if isXPresent && isYPresent && isZPresent {
		x, xParseErr := strconv.Atoi(xParam)
		y, yParseErr := strconv.Atoi(yParam)
		z, zParseErr := strconv.Atoi(zParam)

		if xParseErr != nil || yParseErr != nil || zParseErr != nil {
			return nil, errors.New("Coordinates parse error")
		}

		return &gosm.Tile{X: x, Y: y, Z: z}, nil
	}

	return nil, errors.New("Invalid coordinates param (must be x,y,z)")
}

func main() {
	r := mux.NewRouter()

	r.HandleFunc("/{z}/{x}/{y}.png", tileHandler).Methods("GET")
	http.Handle("/", r)

	log.Fatal(http.ListenAndServe(":8000", r))
}

func tileHandler(w http.ResponseWriter, r *http.Request) {
	params := mux.Vars(r)

	vector, err := newVectorFromMapParams(params)

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	b, err := loadTile(*vector, "tiles")

	if err != nil {
		http.Error(w, "Cannot load tile.", http.StatusInternalServerError)
	}

	createDirIfNotExists("tiles")

	// TODO Fix this path to work on other OSes
	writeTileInDisk("tiles/"+createTileFilename(vector.X, vector.Y, vector.Z), b)

	w.Header().Add("Content-Type", "image/png")
	w.Header().Add("Content-Disposition", fmt.Sprintf("inline; filename=\"%d.png\"", vector.Y))
	w.Write(b)
}

func downloadTilesInBoundingBoxHandler(w http.ResponseWriter, r *http.Request) {
	params := mux.Vars(r)
	top, bottom, err := parseBoundingBoxParams(params)

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
	}

	zoomLevel, err := strconv.Atoi(params["zoom_level"])

	if err != nil {
		http.Error(w, "Zoom level is invalid", http.StatusBadRequest)
	}

	numberOfTiles, err := downloadTilesInBoundingBox(*top, *bottom, zoomLevel, OSM_TILES_URI)

	if err != nil {
		if err == errDownloadTilesLimit {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		http.Error(w, err.Error(), http.StatusInternalServerError)
	}

	msg := fmt.Sprintf("%d tile(s) processed", numberOfTiles)
	w.Write([]byte(msg))
}

func downloadTilesInBoundingBox(top gosm.Tile, bottom gosm.Tile, zoomLevel int, mapUri string) (int, error) {
	tiles, err := listTilesInABoundingBox(top, bottom, zoomLevel)

	if err != nil {
		return 0, err
	}

	if len(tiles) >= 250 && zoomLevel > 13 {
		return 0, errDownloadTilesLimit
	}

	numberOfTiles := 0

	for _, t := range tiles {
		_, err := loadTile(*t, "tiles")
		if err == nil {
			numberOfTiles++
		}
	}

	return numberOfTiles, nil
}

func loadTile(v gosm.Tile, dir string) ([]byte, error) {
	// TODO Fix this path to work on other OSes
	filename := dir + "/" + createTileFilename(v.X, v.Y, v.Z)

	if checkIfFileExists(filename) {
		return os.ReadFile(filename)
	}

	b, err := loadTileFromMapProvider(v, OSM_TILES_URI)

	if err != nil {
		return nil, err
	}

	return b, err
}

func loadTileFromMapProvider(v gosm.Tile, mapUri string) ([]byte, error) {
	tileUri := createTileUri(v.X, v.Y, v.Z, mapUri)

	client := &http.Client{}
	req, _ := http.NewRequest("GET", tileUri, nil)
	req.Header.Set("User-Agent", getUserAgent())

	resp, err := client.Do(req)

	if err != nil {
		return nil, err
	}

	defer resp.Body.Close()

	b, err := io.ReadAll(resp.Body)

	if resp.Status != "200 OK" {
		return nil, errors.New(string(b))
	}

	if err != nil {
		return nil, err
	}

	return b, nil
}

func listTilesInABoundingBox(top gosm.Tile, bottom gosm.Tile, zoomLevel int) ([]*gosm.Tile, error) {
	t1 := gosm.NewTileWithLatLong(top.Lat, top.Long, zoomLevel)
	t2 := gosm.NewTileWithLatLong(top.Lat, top.Long, zoomLevel)

	return gosm.BBoxTiles(*t1, *t2)
}

func writeTileInDisk(filename string, b []byte) error {
	err := os.WriteFile(filename, b, 0700)

	if err != nil {
		log.Printf("Cannot save tile %s in the disk, Cause: %s", filename, err.Error())
		return err
	}

	return nil
}

func createDirIfNotExists(dir string) error {
	if !checkIfFileExists(dir) {
		err := os.Mkdir(dir, 0700)

		if err != nil {
			log.Printf("Cannot create tiles directory, Cause: %s", err.Error())
			return err
		}
	}

	return nil
}

func checkIfFileExists(name string) bool {
	_, err := os.Stat(name)

	if err == nil {
		return true
	}

	if errors.Is(err, os.ErrNotExist) {
		return false
	}

	return false
}

func parseBoundingBoxParams(params map[string]string) (*gosm.Tile, *gosm.Tile, error) {
	tLat, tLon, bLat, bLon := params["topLat"], params["topLon"], params["bottomLat"], params["bottomLon"]

	topLat, errTLat := strconv.ParseFloat(tLat, 32)
	topLon, errTLon := strconv.ParseFloat(tLon, 32)
	bottomLat, errBLat := strconv.ParseFloat(bLat, 32)
	bottomLon, errBLon := strconv.ParseFloat(bLon, 32)

	if errTLat != nil || errTLon != nil || errBLat != nil || errBLon != nil {
		return nil, nil, errors.New("Invalid bounding box values")
	}

	return &gosm.Tile{Lat: topLat, Long: topLon}, &gosm.Tile{Lat: bottomLat, Long: bottomLon}, nil
}

func createTileUri(x, y, z int, uri string) string {
	return fmt.Sprintf(uri, z, x, y)
}

func createTileFilename(x, y, z int) string {
	return fmt.Sprintf("%d-%d-%d.png", x, y, z)
}

func getUserAgent() string {
	return fmt.Sprintf("osm-cache/v0.0.1 (%s)", runtime.GOOS)
}
