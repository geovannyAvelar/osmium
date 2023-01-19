package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"osm-cache/internal"

	"github.com/go-chi/chi/v5"

	log "github.com/sirupsen/logrus"
)

func main() {
	osm := internal.Provider{
		Url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
		Dir: "tiles/osm",
	}

	api := internal.HttpApi{
		Router:         chi.NewRouter(),
		BasePath:       getRootPath(),
		Providers:      map[string]internal.Provider{"osm": osm},
		AllowedOrigins: getAllowedOrigins(),
	}

	api.Run(getApiPort())
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

func getLocalHost() string {
	logLevel := log.GetLevel()

	log.SetLevel(0)
	port := getApiPort()
	log.SetLevel(logLevel)

	return fmt.Sprintf("http://localhost:%d", port)
}

func getRootPath() string {
	root := os.Getenv("OSM_BASE_PATH")

	if root != "" && len(root) > 0 {
		if root[0] == '/' {
			return root
		}
	}

	log.Warn("OSM_BASE_PATH enviroment variable is not defined. Default is /")

	return "/"
}
