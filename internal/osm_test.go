package internal

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

var payload = []byte{1, 2, 3}
var osmServer = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
	formatSplit := strings.Split(r.URL.Path, ".")
	vars := strings.Split(formatSplit[0][1:], "/")
	format := formatSplit[1]

	x := vars[0]
	y := vars[1]
	z := vars[2]

	if x == "0" && y == "0" && z == "0" && (format == "png" || format == "jpg") {
		w.Write(payload)
		return
	}

	http.Error(w, "Tile not found", http.StatusNotFound)
}))

func TestGetTile(t *testing.T) {
	t.Parallel()

	provider := Provider{
		Url: osmServer.URL + "/{z}/{x}/{y}.{format}",
		Dir: "testdata/savetest",
	}

	tile, err := provider.GetTile(0, 0, 0, Png)

	if err != nil {
		t.Errorf("Cannot get tile. Cause: %s", err)
		return
	}

	if bytes.Compare(tile.Bytes, payload) != 0 {
		t.Errorf("Returned bytes are not equal. Received %b but expected %b", tile.Bytes, payload)
	}

	os.Remove(formatTilePath("testdata/savetest", 0, 0, 0, Png))
}

func TestDownloadTile(t *testing.T) {
	t.Parallel()

	provider := Provider{
		Url: osmServer.URL + "/{z}/{x}/{y}.{format}",
	}

	b, err := provider.downloadTile(0, 0, 0, Png, map[string][]string{})

	if err != nil {
		t.Errorf("Cannot download tile. Cause: %s", err)
	}

	if bytes.Compare(b, payload) != 0 {
		t.Errorf("Returned bytes are not equal. Received %b but expected %b", b, payload)
	}

	_, err = provider.downloadTile(1, 1, 1, Jpg, map[string][]string{})

	if err == nil {
		t.Errorf("Inexistent tile found. %s", err)
	}
}

func TestSaveTile(t *testing.T) {
	t.Parallel()

	provider := Provider{
		Dir: "testdata/savetest",
	}

	path, err := provider.saveTile(0, 0, 0, Png, []byte{1, 2, 3})

	if err != nil {
		t.Errorf("Cannot save tile. Cause: %s", err)
	}

	os.Remove(path)
}

func TestGetTileFromDisk(t *testing.T) {
	t.Parallel()

	provider := Provider{
		Dir: "testdata/savetest",
	}

	path, err := provider.saveTile(0, 0, 0, Png, []byte{1, 2, 3})

	if err != nil {
		t.Errorf("Cannot save tile. Cause: %s", err)
		return
	}

	tile, err := provider.getTileFromDisk(0, 0, 0, Png)

	if err != nil {
		t.Errorf("Cannot read tile from disk. Cause: %s", err)
	}

	if bytes.Compare(tile.Bytes, payload) != 0 {
		t.Errorf("Returned bytes are not equal. Received %b but expected %b", tile.Bytes, payload)
	}

	os.Remove(path)
}
