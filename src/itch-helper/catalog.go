// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path"
	"regexp"
	"strconv"
	"strings"
	"sync"

	"golang.org/x/net/html"
)

var consoleTags = map[string]string{
	"gb": "tag-gameboy-rom", "gbc": "tag-gameboy-color", "gba": "tag-gameboy-advance",
}
var genres = map[string]bool{
	"all": true, "action": true, "adventure": true, "platformer": true, "puzzle": true,
	"rpg": true, "shooter": true, "simulation": true, "strategy": true,
}
var sortPaths = map[string]string{"popular": "", "newest": "newest", "top-rated": "top-rated"}
var qualifiedROM = regexp.MustCompile(`(?i)\bfor\s+(gba|gbc|gb)\s*[(]roms?\b`)
var namedROM = regexp.MustCompile(`(?i)\b(gba|gbc|gb|game\s*boy(?:\s+(?:advance|color|colour))?)\s+rom\b`)

type catalogGame struct {
	id                                    int64
	title, creator, price, genre, address string
}

func nodeAttr(n *html.Node, name string) string {
	for _, attr := range n.Attr {
		if attr.Key == name {
			return attr.Val
		}
	}
	return ""
}

func nodeClass(n *html.Node, class string) bool {
	for _, part := range strings.Fields(nodeAttr(n, "class")) {
		if part == class {
			return true
		}
	}
	return false
}

func nodeText(n *html.Node) string {
	var text strings.Builder
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Data == "svg" {
			return
		}
		if node.Type == html.TextNode {
			text.WriteString(node.Data)
		}
		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(n)
	return strings.TrimSpace(text.String())
}

func catalogEntries(raw []byte) ([]catalogGame, bool, error) {
	root, err := html.Parse(bytes.NewReader(raw))
	if err != nil {
		return nil, false, err
	}
	var entries []catalogGame
	seen := map[int64]bool{}
	more, grid := false, false
	var walk func(*html.Node, *catalogGame)
	walk = func(n *html.Node, game *catalogGame) {
		var entry catalogGame
		cell := n.Type == html.ElementNode && nodeClass(n, "game_cell")
		if cell {
			entry.id, _ = strconv.ParseInt(nodeAttr(n, "data-game_id"), 10, 64)
			game = &entry
		}
		if n.Type == html.ElementNode {
			if nodeClass(n, "browse_game_grid") {
				grid = true
			}
			if n.Data == "link" && nodeAttr(n, "rel") == "next" {
				more = true
			}
			if game != nil {
				switch {
				case n.Data == "a" && nodeClass(n, "title") && nodeClass(n, "game_link"):
					game.address = nodeAttr(n, "href")
					game.title = nodeText(n)
				case nodeClass(n, "game_author"):
					game.creator = nodeText(n)
				case nodeClass(n, "price_value"):
					game.price = nodeText(n)
				case nodeClass(n, "game_genre"):
					game.genre = nodeText(n)
				}
			}
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child, game)
		}
		if cell && entry.id > 0 && !seen[entry.id] && len(entries) < 80 {
			u, ok := project(entry.address)
			if ok && len(entry.address) < 250 && entry.title != "" {
				if entry.creator == "" {
					entry.creator = strings.TrimSuffix(u.Hostname(), ".itch.io")
				}
				entry.address = strings.TrimRight(entry.address, "/")
				seen[entry.id] = true
				entries = append(entries, entry)
			}
		}
	}
	walk(root, nil)
	// Itch uses an empty results message in place of the grid for an empty filter.
	if !grid && len(entries) == 0 && !bytes.Contains(raw, []byte("browse_game_grid")) &&
		!bytes.Contains(raw, []byte("No games")) && !bytes.Contains(raw, []byte("no_results")) &&
		!bytes.Contains(raw, []byte("0 results")) {
		return nil, false, errors.New("Could not read the itch.io catalog")
	}
	return entries, more, nil
}

func searchWords(value string) []string {
	return strings.Fields(strings.NewReplacer("-", " ", "_", " ").Replace(strings.ToLower(value)))
}

func matchesQuery(game catalogGame, query string) bool {
	words := strings.Join(searchWords(game.title+" "+game.creator+" "+game.address), " ")
	for _, term := range searchWords(query) {
		if !strings.Contains(words, term) {
			return false
		}
	}
	return true
}

func uploadConsole(name string) string {
	ext := strings.ToLower(path.Ext(name))
	if supported(ext) {
		return ext[1:]
	}
	m := namedROM.FindStringSubmatch(name)
	if len(m) == 0 {
		m = qualifiedROM.FindStringSubmatch(name)
		if len(m) == 0 {
			return ""
		}
	}
	format := strings.Join(strings.Fields(strings.ToLower(m[1])), " ")
	switch format {
	case "gba", "gameboy advance", "game boy advance":
		return "gba"
	case "gbc", "gameboy color", "game boy color", "gameboy colour", "game boy colour":
		return "gbc"
	default:
		return "gb"
	}
}

func consoleMatches(file, system string) bool {
	return file == system || (system == "gbc" && file == "gb")
}

// ZIP-only projects need a directory check. The CDN must support a bounded
// suffix range. If the archive cannot be inspected, skip it in the catalog.
const zipTailSize = 128 << 10

type zipTail struct {
	data   []byte
	offset int64
}

func (tail zipTail) ReadAt(p []byte, offset int64) (int, error) {
	offset -= tail.offset
	if offset < 0 || offset >= int64(len(tail.data)) {
		return 0, io.EOF
	}
	n := copy(p, tail.data[int(offset):])
	if n != len(p) {
		return n, io.EOF
	}
	return n, nil
}

func archiveHasROM(c *http.Client, address, system string) bool {
	req, err := http.NewRequest(http.MethodGet, address, nil)
	if err != nil || !permitted(req.URL) {
		return false
	}
	req.Header.Set("User-Agent", agent)
	req.Header.Set("Accept-Encoding", "identity")
	req.Header.Set("Range", fmt.Sprintf("bytes=-%d", zipTailSize))
	res, err := c.Do(req)
	if err != nil {
		return false
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusPartialContent {
		return false
	}
	var start, end, size int64
	if n, _ := fmt.Sscanf(res.Header.Get("Content-Range"), "bytes %d-%d/%d", &start, &end, &size); n != 3 || start < 0 || size <= 0 || size > maxArchive || end != size-1 || end-start+1 > zipTailSize {
		return false
	}
	raw, err := limited(res.Body, zipTailSize)
	if err != nil || int64(len(raw)) != end-start+1 {
		return false
	}
	reader, err := zip.NewReader(zipTail{data: raw, offset: start}, size)
	if err != nil {
		return false
	}
	for _, file := range reader.File {
		ext := strings.ToLower(path.Ext(file.Name))
		if supported(ext) && consoleMatches(ext[1:], system) && file.Mode().IsRegular() &&
			!strings.HasPrefix(path.Base(file.Name), ".") && !strings.HasPrefix(file.Name, "__MACOSX/") &&
			file.UncompressedSize64 >= 192 && file.UncompressedSize64 <= maxROM {
			return true
		}
	}
	return false
}

func catalogHasROM(c *http.Client, game catalogGame, system string) (bool, error) {
	raw, err := page(c, game.address, nil, "")
	if err != nil {
		return false, err
	}
	hasZip := false
	for _, file := range uploads(raw) {
		if consoleMatches(uploadConsole(file.name), system) {
			return true, nil
		}
		if strings.EqualFold(path.Ext(file.name), ".zip") {
			hasZip = true
		}
	}
	if !hasZip || !isFree(game.price) {
		return false, nil
	}
	// A separate cookie jar keeps guest download sessions for projects on the
	// same creator domain from interfering with one another.
	guest := *c
	guest.Jar = newClient().Jar
	files, signed, csrf, key, err := guestUploads(&guest, game.address)
	if err != nil {
		return false, err
	}
	tried := 0
	for _, file := range files {
		if file.id == "" {
			continue
		}
		if consoleMatches(uploadConsole(file.name), system) {
			return true, nil
		}
		if !strings.EqualFold(path.Ext(file.name), ".zip") || tried >= 2 {
			continue
		}
		tried++
		cdn, err := postURL(&guest, game.address+"/file/"+file.id, signed,
			url.Values{"csrf_token": {csrf}, "download_key_id": {key}})
		if err == nil && archiveHasROM(&guest, cdn, system) {
			return true, nil
		}
	}
	return false, nil
}

func isFree(price string) bool {
	label := priceLabel(price)
	return label == "Free" || label == "Free / name your price"
}

func matchesGenre(game catalogGame, genre string) bool {
	if genre == "all" {
		return true
	}
	label := strings.ToLower(game.genre)
	if genre == "rpg" && (label == "role playing" || label == "role-playing") {
		return true
	}
	return label == genre
}

func list(c *http.Client, system, price, genre, order string, number int, query string, output io.Writer) error {
	tag := consoleTags[system]
	sortPath, hasSort := sortPaths[order]
	if tag == "" || !genres[genre] || !hasSort || number < 1 || number > 100 {
		return errVisit
	}
	if price != "all" && price != "free" && price != "paid" {
		return errVisit
	}
	parts := []string{"https://itch.io/games"}
	for _, part := range []string{sortPath, tag} {
		if part != "" && part != "all" {
			parts = append(parts, part)
		}
	}
	raw, err := page(c, fmt.Sprintf("%s?page=%d", strings.Join(parts, "/"), number), nil, "")
	if err != nil {
		return err
	}
	entries, more, err := catalogEntries(raw)
	if err != nil {
		return err
	}
	type result struct {
		rom bool
		err error
	}
	checked := make([]result, len(entries))
	var workers sync.WaitGroup
	jobs := make(chan int)
	for range 4 {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for i := range jobs {
				checked[i].rom, checked[i].err = catalogHasROM(c, entries[i], system)
			}
		}()
	}
	attempted := 0
	for index, game := range entries {
		if (price == "all" || (price == "free") == isFree(game.price)) &&
			matchesGenre(game, genre) && matchesQuery(game, query) {
			jobs <- index
			attempted++
		}
	}
	close(jobs)
	workers.Wait()
	matched, failed := 0, 0
	for _, result := range checked {
		if result.rom {
			matched++
		}
		if result.err != nil && !errors.Is(result.err, errVisit) {
			failed++
		}
	}
	if matched == 0 && failed > 0 && failed == attempted {
		return errors.New("Couldn't verify this page. Retry to check the ROMs")
	}
	if more && number < 100 {
		fmt.Fprintln(output, "P 1")
	} else {
		fmt.Fprintln(output, "P 0")
	}
	for i, game := range entries {
		if checked[i].rom {
			fmt.Fprintf(output, "%d\t%s\t%s\t%s\t%s\n", game.id, clean(game.title, 95),
				clean(game.creator, 45), priceLabel(game.price), game.address)
		}
	}
	return nil
}
