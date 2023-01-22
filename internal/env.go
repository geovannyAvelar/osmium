package internal

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	log "github.com/sirupsen/logrus"
)

func GetAllowedOrigins() []string {
	envVar := os.Getenv("OSM_CACHE_ALLOWED_ORIGINS")

	if envVar != "" {
		return strings.Split(envVar, ",")
	}

	log.Warn("OSM_CACHE_ALLOWED_ORIGINS enviroment variable is not defined. Accepting only local connections")

	return []string{GetLocalHost()}
}

func GetApiPort() int {
	envVar := os.Getenv("OSM_PORT")

	if envVar != "" {
		port, err := strconv.Atoi(envVar)

		if err == nil {
			return port
		}

		log.Warn("Cannot parse OSM_PORT enviroment variable. Port must be an integer.")
	}

	log.Warn("OSM_PORT is not defined.")
	log.Warn("Using default port 8000.")

	return 8000
}

func GetLocalHost() string {
	logLevel := log.GetLevel()

	log.SetLevel(0)
	port := GetApiPort()
	log.SetLevel(logLevel)

	return fmt.Sprintf("http://localhost:%d", port)
}

func GetRootPath() string {
	root := os.Getenv("OSM_BASE_PATH")

	if root != "" && len(root) > 0 {
		if root[0] == '/' {
			return root
		}
	}

	log.Warn("OSM_BASE_PATH enviroment variable is not defined. Default is /")

	return "/"
}

func GetTilesPath() string {
	path := os.Getenv("OSM_TILES_PATH")

	if path != "" {
		return path
	}

	log.Warn("OSM_TILES_PATH enviroment variable is not defined." +
		"Tiles will be stored in ./tiles folder")

	return "tiles"
}
