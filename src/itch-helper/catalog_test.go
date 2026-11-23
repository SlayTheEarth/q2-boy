// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"archive/zip"
	"bytes"
	"fmt"
	"io"
	"net/http"
	"testing"
)

func TestCatalogSearchAndFilters(t *testing.T) {
	raw := []byte(`<link rel="next" href="?page=2"><div class="browse_game_grid">
		<div class="game_cell" data-game_id="42">
		<a href="https://dev-name.itch.io/furry-friends" class="title game_link">Furry Friends</a>
		<div class="game_author"><a>Jane &amp; Co</a><svg><title>Verified Account</title></svg></div>
		<div class="price_value">$5</div><div class="game_genre">Role Playing</div>
		</div></div>`)
	games, more, err := catalogEntries(raw)
	if err != nil || !more || len(games) != 1 {
		t.Fatalf("games=%v more=%v err=%v", games, more, err)
	}
	g := games[0]
	if g.id != 42 || g.creator != "Jane & Co" || isFree(g.price) ||
		!matchesGenre(g, "rpg") || matchesGenre(g, "puzzle") ||
		!matchesQuery(g, "jane friends") || !matchesQuery(g, "dev name") ||
		matchesQuery(g, "verified") {
		t.Fatalf("incorrect filters or search for %+v", g)
	}
}

type roundTrip func(*http.Request) (*http.Response, error)

func (fn roundTrip) RoundTrip(req *http.Request) (*http.Response, error) { return fn(req) }

func TestVerifyZIP(t *testing.T) {
	for _, test := range []struct {
		member string
		want   bool
	}{{"bundle/Game.gba", true}, {"index.html", false}, {"bin/game.exe", false}, {"Game.gbc", false}} {
		var body bytes.Buffer
		zw := zip.NewWriter(&body)
		w, err := zw.Create(test.member)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = w.Write(make([]byte, 256)); err != nil {
			t.Fatal(err)
		}
		if err = zw.Close(); err != nil {
			t.Fatal(err)
		}
		size := body.Len()
		client := &http.Client{Transport: roundTrip(func(req *http.Request) (*http.Response, error) {
			if req.Header.Get("Range") == "" {
				t.Error("expected a bounded range")
			}
			headers := make(http.Header)
			headers.Set("Content-Range", fmt.Sprintf("bytes 0-%d/%d", size-1, size))
			return &http.Response{StatusCode: http.StatusPartialContent, Header: headers, Body: io.NopCloser(bytes.NewReader(body.Bytes()))}, nil
		})}
		if got := archiveHasROM(client, "https://files.itch.zone/sample.zip", "gba"); got != test.want {
			t.Errorf("%q verified=%v, want %v", test.member, got, test.want)
		}
	}
	if uploadConsole("Goodboy Galaxy (GBA ROM)") != "gba" ||
		uploadConsole("Demo for GBA (roms only)") != "gba" || uploadConsole("Browser build.zip") != "" {
		t.Error("unexpected public upload classification")
	}
}
