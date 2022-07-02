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

	"github.com/gorilla/mux"
)

func newVectorFromMapParams(params map[string]string) (*Vector, error) {
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

		return &Vector{x, y, z}, nil
	}

	return nil, errors.New("Invalid coordinates param (must be x,y,z)")
}

type Vector struct {
	x int
	y int
	z int
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

	b, err := loadTile(vector)

	if err != nil {
		http.Error(w, "Cannot load tile.", http.StatusInternalServerError)
	}

	w.Header().Add("Content-Type", "image/png")
	w.Header().Add("Content-Disposition", fmt.Sprintf("inline; filename=\"%d.png\"", vector.y))
	w.Write(b)
}

func loadTile(v *Vector) ([]byte, error) {
	filename := fmt.Sprintf("tiles/%d-%d-%d.png", v.x, v.y, v.z)

	if checkIfFileExists(filename) {
		return os.ReadFile(filename)
	}

	b, err := loadTileFromOsm(v)

	if err != nil {
		return nil, err
	}

	writeTileInDisk(filename, b)

	return b, err
}

func loadTileFromOsm(v *Vector) ([]byte, error) {
	tileUri := fmt.Sprintf("https://tile.openstreetmap.org/%d/%d/%d.png", v.z, v.x, v.y)

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

func writeTileInDisk(filename string, b []byte) {
	go func(fileBytes []byte) {
		if !checkIfFileExists("tiles") {
			err := os.Mkdir("tiles", 0644)

			if err != nil {
				log.Printf("Cannot create tiles directory, Cause: %s", err.Error())
			}
		}

		err := os.WriteFile(filename, fileBytes, 0644)

		if err != nil {
			log.Printf("Cannot save tile %s in the disk, Cause: %s", filename, err.Error())
		}
	}(b)
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

func getUserAgent() string {
	return fmt.Sprintf("osm-cache/v0.0.1 (%s)", runtime.GOOS)
}
