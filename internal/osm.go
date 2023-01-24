package internal

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"strings"

	log "github.com/sirupsen/logrus"
)

var errTileNotFound error = errors.New("Tile not found")
var FILE_PATH_SEP = strings.ReplaceAll(strconv.QuoteRune(os.PathSeparator), "'", "")

type TileFormat string

const (
	Png TileFormat = "png"
	Jpg TileFormat = "jpg"
)

type Tile struct {
	X, Y, Z int
	Bytes   []byte
}

type ConfigParam struct {
	Name   string
	Values []string
}

type Provider struct {
	Url         string
	Dir         string
	Attribution string
}

func (o *Provider) GetTile(x int, y int, z int, format TileFormat, params ...*ConfigParam) (*Tile, error) {
	tile, err := o.getTileFromDisk(x, y, z, format)

	if err == nil {
		return tile, nil
	}

	paramsMap := convertConfigParamsToMap(params...)
	bytes, err := o.downloadTile(x, y, z, format, paramsMap)

	if err != nil {
		log.Errorf("Cannot get tile from provider. Cause %s", err)
		return nil, fmt.Errorf("cannot get tile from provider. Cause %w", err)
	}

	_, err = o.saveTile(x, y, z, format, bytes)

	if err != nil {
		log.Warnf("Cannot save tile %d/%d/%d in the disk. Cause: %s", x, y, z, err)
	}

	tile = &Tile{
		x, y, z, bytes,
	}

	return tile, nil
}

func (o *Provider) downloadTile(x int, y int, z int, format TileFormat, params map[string][]string) ([]byte, error) {
	uri := formatUrl(o.Url, x, y, z, format)

	client := &http.Client{}
	req, _ := http.NewRequest("GET", uri, nil)
	req.Header.Set("User-Agent", getUserAgent())

	q := req.URL.Query()

	for key, value := range params {
		for _, v := range value {
			q.Add(key, v)
		}
	}

	req.URL.RawQuery = q.Encode()

	resp, err := client.Do(req)

	if err != nil {
		log.Errorf("Cannot download tile %d/%d/%d.%s Cause: %s", z, x, y, string(format), err)
		return nil, fmt.Errorf("cannot download tile %d/%d/%d. Cause: %w", z, x, y, err)
	}

	defer resp.Body.Close()

	b, err := io.ReadAll(resp.Body)

	status := resp.StatusCode

	if status == http.StatusNotFound {
		return nil, errTileNotFound
	}

	if status > http.StatusBadRequest && status < http.StatusNetworkAuthenticationRequired {
		return nil, errors.New(string(b))
	}

	if err == nil {
		return b, nil
	}

	return nil, err
}

func (o *Provider) saveTile(x int, y int, z int, format TileFormat, bytes []byte) (string, error) {
	dir := formatTileDirPath(o.Dir, x, z)
	err := os.MkdirAll(dir, os.ModePerm)

	if err != nil {
		return "", fmt.Errorf("cannot create directories to store tiles. Cause: %w", err)
	}

	filepath := fmt.Sprintf("%s/%d.png", dir, y)

	if _, err := os.Stat(filepath); err == nil {
		return filepath, nil
	}

	err = os.WriteFile(filepath, bytes, 0644)

	if err != nil {
		return "", fmt.Errorf("cannot create tile file. Cause: %w", err)
	}

	return filepath, nil
}

func (o *Provider) getTileFromDisk(x, y, z int, format TileFormat) (*Tile, error) {
	path := formatTilePath(o.Dir, z, x, y, format)

	if _, err := os.Stat(path); err != nil {
		return nil, errors.New("Tile is not cached")
	}

	bytes, err := os.ReadFile(path)

	if err != nil {
		return nil, fmt.Errorf("cannot read tile from disk. Cause: %w", err)
	}

	return &Tile{
		x, y, z, bytes,
	}, nil
}

func formatUrl(url string, x, y, z int, format TileFormat) string {
	url = strings.ReplaceAll(url, "{x}", strconv.Itoa(x))
	url = strings.ReplaceAll(url, "{y}", strconv.Itoa(y))
	url = strings.ReplaceAll(url, "{z}", strconv.Itoa(z))
	url = strings.ReplaceAll(url, "{format}", string(format))

	return url
}

func formatTilePath(dir string, x, y, z int, format TileFormat) string {
	dir = formatTileDirPath(dir, z, x)
	return fmt.Sprintf("%s%s%d.%s", dir, FILE_PATH_SEP, y, string(format))
}

func formatTileDirPath(dir string, x, z int) string {
	return fmt.Sprintf("%s%s%d%s%d", dir, FILE_PATH_SEP, z, FILE_PATH_SEP, x)
}

func getUserAgent() string {
	return fmt.Sprintf("osm-cache (%s)", runtime.GOOS)
}

func convertConfigParamsToMap(params ...*ConfigParam) map[string][]string {
	m := make(map[string][]string, len(params))

	for _, p := range params {
		m[p.Name] = p.Values
	}

	return m
}
