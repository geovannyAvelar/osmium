package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strconv"
	"strings"

	"github.com/apeyroux/gosm"
	"github.com/gorilla/handlers"
	"github.com/gorilla/mux"

	log "github.com/sirupsen/logrus"
)

const OSM_TILES_URI = "https://tile.openstreetmap.org/%d/%d/%d.png"
const TILES_DIR_PATH = "tiles"

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
	r.HandleFunc("/update-tiles", downloadTilesInBoundingBoxHandler).Methods("POST")
	http.Handle("/", r)

	headersOk := handlers.AllowedHeaders([]string{"X-Requested-With"})
	originsOk := handlers.AllowedOrigins(getAllowedOrigins())
	methodsOk := handlers.AllowedMethods([]string{"GET", "HEAD", "POST", "PUT", "OPTIONS"})

	handler := handlers.CORS(originsOk, headersOk, methodsOk)(r)

	host := fmt.Sprintf(":%d", getApiPort())

	log.Info("Listening at " + host)

	log.Fatal(http.ListenAndServe(host, handler))
}

func tileHandler(w http.ResponseWriter, r *http.Request) {
	params := mux.Vars(r)

	vector, err := newVectorFromMapParams(params)

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	_, b, err := loadTile(*vector, TILES_DIR_PATH)

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
	params := r.URL.Query()
	top, bottom, err := parseBoundingBoxParams(params)

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	tiles, err := downloadTilesInBoundingBox(*top, *bottom, OSM_TILES_URI, TILES_DIR_PATH)

	if err != nil {
		if err == errDownloadTilesLimit {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	json.NewEncoder(w).Encode(tiles)
}

func downloadTilesInBoundingBox(top gosm.Tile, bottom gosm.Tile, mapUri, tilesDir string) ([]string, error) {
	tiles, err := listTilesInABoundingBox(top, bottom)

	if err != nil {
		return nil, err
	}

	if len(tiles) >= 250 {
		return nil, errDownloadTilesLimit
	}

	tilesProcessed := []string{}

	for _, t := range tiles {
		filepath, b, err := loadTile(*t, tilesDir)
		if err == nil {
			if checkIfFileExists(filepath) {
				continue
			}

			err := createDirIfNotExists(tilesDir)

			if err == nil {
				err := writeTileInDisk(filepath, b)

				if err == nil {
					tilesProcessed = append(tilesProcessed, filepath)
				}
			}
		}
	}

	return tilesProcessed, nil
}

func loadTile(v gosm.Tile, dir string) (string, []byte, error) {
	// TODO Fix this path to work on other OSes
	filepath := dir + "/" + createTileFilename(v.X, v.Y, v.Z)

	if checkIfFileExists(filepath) {
		b, err := os.ReadFile(filepath)
		return filepath, b, err
	}

	b, err := loadTileFromMapProvider(v, OSM_TILES_URI)

	if err != nil {
		return "", nil, err
	}

	return filepath, b, err
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

func listTilesInABoundingBox(top gosm.Tile, bottom gosm.Tile) ([]*gosm.Tile, error) {
	t1 := gosm.NewTileWithLatLong(top.Lat, top.Long, 19)
	t2 := gosm.NewTileWithLatLong(top.Lat, top.Long, 19)

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

func parseBoundingBoxParams(params url.Values) (*gosm.Tile, *gosm.Tile, error) {
	topLat, errTLat := strconv.ParseFloat(params.Get("topLat"), 32)
	topLon, errTLon := strconv.ParseFloat(params.Get("topLon"), 32)
	bottomLat, errBLat := strconv.ParseFloat(params.Get("bottomLat"), 32)
	bottomLon, errBLon := strconv.ParseFloat(params.Get("bottomLon"), 32)

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

func getAllowedOrigins() []string {
	envVar := os.Getenv("OSM_CACHE_ALLOWED_ORIGINS")

	if envVar != "" {
		return strings.Split(envVar, ",")
	}

	log.Warn("OSM_CACHE_ALLOWED_ORIGINS enviroment variable is not defined. Accepting only local connections")

	return []string{getLocalHost()}
}

func getApiPort() int {
	envVar := os.Getenv("PORT")

	if envVar != "" {
		port, err := strconv.Atoi(envVar)

		if err == nil {
			return port
		}

		log.Warn("Cannot parse PORT enviroment variable. Port must be an integer.")
	}

	log.Warn("PORT is not defined.")
	log.Warn("Using default port 8000.")

	return 8000
}

func getLocalHost() string {
	logLevel := log.GetLevel()

	log.SetLevel(0)
	port := getApiPort()
	log.SetLevel(logLevel)

	return fmt.Sprintf("http://localhost:%d", port)
}
