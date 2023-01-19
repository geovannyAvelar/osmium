package internal

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
	"github.com/gorilla/handlers"

	log "github.com/sirupsen/logrus"
)

func GetDefaultApi(origins []string) *HttpApi {
	osm := Provider{
		Url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
		Dir: "tiles/osm",
	}
	providers := map[string]Provider{
		"osm": osm,
	}

	return &HttpApi{
		Router:         chi.NewRouter(),
		Providers:      providers,
		AllowedOrigins: origins,
	}
}

type HttpApi struct {
	Router         *chi.Mux
	Providers      map[string]Provider
	BasePath       string
	AllowedOrigins []string
}

func (a *HttpApi) Run(port int) error {
	if port < 0 || port > 65535 {
		return errors.New("invalid HTTP port")
	}

	if len(a.Providers) == 0 {
		return errors.New("it is necessary to include at least one provider")
	}

	a.Router.Route(a.BasePath, func(r chi.Router) {
		r.Get("/{z}/{x}/{y}.{format}", a.handleTile)
		r.Get("/{provider}/{z}/{x}/{y}.{format}", a.handleTile)
		r.Get("/providers", a.listProviders)
	})

	headersOk := handlers.AllowedHeaders([]string{"X-Requested-With"})
	originsOk := handlers.AllowedOrigins(a.AllowedOrigins)
	methodsOk := handlers.AllowedMethods([]string{"GET", "HEAD", "POST", "PUT", "OPTIONS"})

	handler := handlers.CORS(originsOk, headersOk, methodsOk)(a.Router)

	host := fmt.Sprintf(":%d", port)
	log.Info("Listening at " + host + a.BasePath)

	return http.ListenAndServe(host, handler)
}

func (a *HttpApi) handleTile(w http.ResponseWriter, r *http.Request) {
	tile, err := a.newTileFromRequest(r)

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	provider, err := a.getProviderByName(chi.URLParam(r, "provider"))

	if err != nil {
		i := 0
		for _, p := range a.Providers {
			if i > 0 {
				break
			}

			provider = &p
			i++
		}
	}

	format, err := a.getTileFormat(chi.URLParam(r, "format"))

	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	tile, err = provider.GetTile(tile.X, tile.Y, tile.Z, format)

	if err != nil {
		http.Error(w, errors.Unwrap(err).Error(), http.StatusInternalServerError)
		return
	}

	formatString := string(format)
	contentDisposition := fmt.Sprintf("inline; filename=\"%d.%s\"", tile.Y, formatString)

	w.Header().Add("Content-Type", "image/"+formatString)
	w.Header().Add("Content-Disposition", contentDisposition)
	w.Write(tile.Bytes)
}

func (a *HttpApi) listProviders(w http.ResponseWriter, r *http.Request) {
	providerNames := make([]string, len(a.Providers))

	i := 0
	for name := range a.Providers {
		providerNames[i] = name
		i++
	}

	json.NewEncoder(w).Encode(providerNames)
}

func (a *HttpApi) newTileFromRequest(r *http.Request) (*Tile, error) {
	xParam := chi.URLParam(r, "x")
	yParam := chi.URLParam(r, "y")
	zParam := chi.URLParam(r, "z")

	if xParam != "" && yParam != "" && zParam != "" {
		x, xParseErr := strconv.Atoi(xParam)
		y, yParseErr := strconv.Atoi(yParam)
		z, zParseErr := strconv.Atoi(zParam)

		if xParseErr != nil || yParseErr != nil || zParseErr != nil {
			return nil, errors.New("coordinates parse error")
		}

		return &Tile{X: x, Y: y, Z: z}, nil
	}

	return nil, errors.New("invalid coordinates param (must be x,y,z)")
}

func (a *HttpApi) getProviderByName(name string) (*Provider, error) {
	if val, ok := a.Providers[name]; ok {
		return &val, nil
	}

	return nil, errors.New("Cannot find provider with name " + name)
}

func (a *HttpApi) getTileFormat(name string) (TileFormat, error) {
	switch name {
	case "png", "PNG":
		return Png, nil
	case "jpg", "JPG", "jpeg":
		return Jpg, nil
	}

	return "", errors.New("Invalid format " + name)
}
