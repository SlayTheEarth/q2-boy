// SPDX-License-Identifier: GPL-2.0-or-later
// itch.io's guest download flow and upload markup were adapted from
// NextUI-Itchio-Pak. See NextUI-Itchio-Pak-LICENSE.
package main

import (
	"archive/zip"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	"image/png"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/skip2/go-qrcode"
	_ "golang.org/x/image/webp"
	"golang.org/x/net/html"
)

const maxROM = 32 << 20
const maxArchive = 48 << 20
const maxPage = 2 << 20
const agent = "PocketBoy/0.4.1 (https://github.com/SlayTheEarth/q2-boy)"

var (
	errVisit   = errors.New("Open the itch.io page for this download")
	errNoROM   = errors.New("No supported ROM in the free files")
	errConnect = errors.New("Could not reach itch.io. Check Q2 Wi-Fi")
	errExists  = errors.New("This ROM is already in your library")
	gameSlug   = regexp.MustCompile(`^/[a-zA-Z0-9_-]+/?$`)
	csrfTag    = regexp.MustCompile(`name="csrf_token"\s+(?:content|value)="([^"]+)"`)
)

func permitted(u *url.URL) bool {
	if u == nil || u.Scheme != "https" || u.User != nil || u.Port() != "" {
		return false
	}
	h := u.Hostname()
	return h == "itch.io" || strings.HasSuffix(h, ".itch.io") ||
		h == "itch.zone" || strings.HasSuffix(h, ".itch.zone") ||
		h == "hwcdn.net" || strings.HasSuffix(h, ".hwcdn.net") ||
		(strings.HasPrefix(h, "itchio-") && strings.HasSuffix(h, ".r2.cloudflarestorage.com"))
}

func project(raw string) (*url.URL, bool) {
	u, err := url.Parse(raw)
	return u, err == nil && permitted(u) && strings.HasSuffix(u.Hostname(), ".itch.io") &&
		gameSlug.MatchString(u.Path) && u.RawQuery == "" && u.Fragment == ""
}

func newClient() *http.Client {
	jar, _ := cookiejar.New(nil)
	return &http.Client{Jar: jar, Timeout: 18 * time.Second, CheckRedirect: func(r *http.Request, via []*http.Request) error {
		if len(via) >= 5 || !permitted(r.URL) {
			return errVisit
		}
		return nil
	}}
}

func fetch(c *http.Client, address string, form url.Values, referer string) (*http.Response, error) {
	method := http.MethodGet
	var body io.Reader
	if form != nil {
		method = http.MethodPost
		body = strings.NewReader(form.Encode())
	}
	req, err := http.NewRequest(method, address, body)
	if err != nil || !permitted(req.URL) {
		return nil, errVisit
	}
	req.Header.Set("User-Agent", agent)
	if form != nil || referer != "" {
		req.Header.Set("Cache-Control", "no-cache")
	}
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	if form != nil {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.Header.Set("X-Requested-With", "XMLHttpRequest")
		req.Header.Set("Origin", req.URL.Scheme+"://"+req.URL.Host)
	}
	res, err := c.Do(req)
	if err != nil {
		if errors.Is(err, errVisit) {
			return nil, errVisit
		}
		return nil, errConnect
	}
	if res.StatusCode != http.StatusOK {
		res.Body.Close()
		if res.StatusCode == 429 || res.StatusCode >= 500 {
			return nil, errors.New("itch.io is busy. Try again later")
		}
		return nil, errVisit
	}
	return res, nil
}

func limited(r io.Reader, limit int64) ([]byte, error) {
	b, err := io.ReadAll(io.LimitReader(r, limit+1))
	if err != nil {
		return nil, errConnect
	}
	if int64(len(b)) > limit {
		return nil, errors.New("This file is too large for Pocket Boy")
	}
	return b, nil
}

func page(c *http.Client, address string, form url.Values, referer string) ([]byte, error) {
	res, err := fetch(c, address, form, referer)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	return limited(res.Body, maxPage)
}

func clean(s string, max int) string {
	s = strings.Join(strings.Fields(s), " ")
	if len(s) <= max {
		return s
	}
	for !utf8.RuneStart(s[max]) {
		max--
	}
	return s[:max]
}

func priceLabel(raw string) string {
	s := clean(raw, 26)
	if s == "" || s == "Free" {
		return "Free"
	}
	value := strings.Trim(s, "$€£¥ ")
	if n, err := strconv.ParseFloat(value, 64); err == nil && n == 0 {
		return "Free / name your price"
	}
	return s
}

type gameData struct {
	ID        int64  `json:"id"`
	Title     string `json:"title"`
	Price     string `json:"price"`
	Suggested string `json:"suggested_price"`
	Cover     string `json:"cover_image"`
}

func metadata(c *http.Client, base string) (gameData, error) {
	var data gameData
	raw, err := page(c, base+"/data.json", nil, "")
	if err != nil {
		return data, err
	}
	if json.Unmarshal(raw, &data) != nil || data.ID <= 0 || clean(data.Title, 95) == "" {
		return data, errVisit
	}
	return data, nil
}

type upload struct{ id, name string }

func uploads(doc []byte) []upload {
	root, err := html.Parse(bytes.NewReader(doc))
	if err != nil {
		return nil
	}
	attr := func(n *html.Node, name string) string {
		for _, a := range n.Attr {
			if a.Key == name {
				return a.Val
			}
		}
		return ""
	}
	hasClass := func(n *html.Node, class string) bool {
		return strings.Contains(" "+strings.Join(strings.Fields(attr(n, "class")), " ")+" ", " "+class+" ")
	}
	var out []upload
	var walk func(*html.Node, *upload)
	walk = func(n *html.Node, item *upload) {
		var entry upload
		outer := n.Type == html.ElementNode && n.Data == "div" && hasClass(n, "upload")
		if outer {
			item = &entry
		}
		if item != nil && n.Type == html.ElementNode {
			if n.Data == "a" {
				if id, err := strconv.ParseUint(attr(n, "data-upload_id"), 10, 64); err == nil && id != 0 {
					item.id = strconv.FormatUint(id, 10)
				}
			}
			if n.Data == "strong" && hasClass(n, "name") {
				item.name = attr(n, "title")
				if item.name == "" && n.FirstChild != nil {
					item.name = strings.TrimSpace(n.FirstChild.Data)
				}
			}
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child, item)
		}
		if outer && entry.name != "" {
			out = append(out, entry)
		}
	}
	walk(root, nil)
	return out
}

func token(b []byte) string {
	if m := csrfTag.FindSubmatch(b); len(m) == 2 {
		return string(m[1])
	}
	return ""
}

func postURL(c *http.Client, address, referer string, values url.Values) (string, error) {
	var reply struct {
		URL string `json:"url"`
	}
	raw, err := page(c, address, values, referer)
	if err != nil {
		return "", err
	}
	if json.Unmarshal(raw, &reply) != nil || reply.URL == "" {
		return "", errVisit
	}
	u, err := url.Parse(reply.URL)
	if err != nil || !permitted(u) {
		return "", errVisit
	}
	return reply.URL, nil
}

func keyID(key string) string {
	payload, _, found := strings.Cut(key, ".")
	if !found {
		return ""
	}
	b, err := base64.RawStdEncoding.DecodeString(strings.TrimRight(payload, "="))
	if err != nil {
		b, err = base64.RawURLEncoding.DecodeString(strings.TrimRight(payload, "="))
	}
	var keyData struct {
		ID int64 `json:"id"`
	}
	if err != nil || json.Unmarshal(b, &keyData) != nil || keyData.ID <= 0 {
		return ""
	}
	return strconv.FormatInt(keyData.ID, 10)
}

func guestUploads(c *http.Client, base string) ([]upload, string, string, string, error) {
	// This page supplies a per-visit cookie and its CSRF token. A cached
	// listing page can omit the cookie even if it contains a token.
	session := fmt.Sprintf("%s?pocket_boy_session=%d", base, time.Now().UnixNano())
	doc, err := page(c, session, nil, base)
	if err != nil {
		return nil, "", "", "", err
	}
	csrf := token(doc)
	if csrf == "" {
		return nil, "", "", "", errVisit
	}
	signed, err := postURL(c, base+"/download_url", base,
		url.Values{"csrf_token": {csrf}, "suggested_amount": {"0"}})
	if err != nil {
		return nil, "", "", "", err
	}
	u, _ := url.Parse(signed)
	baseURL, _ := url.Parse(base)
	prefix := baseURL.EscapedPath() + "/download/"
	if u.Host != baseURL.Host || !strings.HasPrefix(u.EscapedPath(), prefix) {
		return nil, "", "", "", errVisit
	}
	key, err := url.PathUnescape(strings.TrimPrefix(u.EscapedPath(), prefix))
	if err != nil || key == "" {
		return nil, "", "", "", errVisit
	}
	doc, err = page(c, signed, nil, base)
	if err != nil {
		return nil, "", "", "", err
	}
	csrf = token(doc)
	if csrf == "" {
		return nil, "", "", "", errVisit
	}
	return uploads(doc), signed, csrf, keyID(key), nil
}

func supported(ext string) bool { return ext == ".gb" || ext == ".gbc" || ext == ".gba" }

func looksLikeROM(data []byte, ext string) bool {
	if ext == ".gba" {
		return len(data) >= 192 && data[0xb2] == 0x96 &&
			(data[3] == 0xea || bytes.Equal(data[4:8], []byte{0x24, 0xff, 0xae, 0x51}))
	}
	return len(data) >= 0x150 && bytes.Equal(data[0x104:0x108], []byte{0xce, 0xed, 0x66, 0x66})
}

func romReader(file *os.File, size int64, ext, preferred string) (io.ReadCloser, string, error) {
	if supported(ext) {
		if size <= maxROM && validHeader(file, ext) {
			if _, err := file.Seek(0, io.SeekStart); err == nil {
				return io.NopCloser(file), ext, nil
			}
		}
	}
	z, err := zip.NewReader(file, size)
	if err != nil {
		return nil, "", errNoROM
	}
	sort.SliceStable(z.File, func(i, j int) bool {
		return strings.EqualFold(path.Ext(z.File[i].Name), preferred) && !strings.EqualFold(path.Ext(z.File[j].Name), preferred)
	})
	for _, f := range z.File {
		name := path.Base(f.Name)
		ext := strings.ToLower(path.Ext(name))
		if !supported(ext) || strings.HasPrefix(name, ".") || strings.HasPrefix(f.Name, "__MACOSX/") ||
			!f.Mode().IsRegular() || f.UncompressedSize64 > maxROM {
			continue
		}
		r, err := f.Open()
		if err != nil {
			continue
		}
		valid := validHeader(r, ext)
		r.Close()
		if valid {
			if r, err = f.Open(); err == nil {
				return r, ext, nil
			}
		}
	}
	return nil, "", errNoROM
}

func validHeader(r io.Reader, ext string) bool {
	var header [0x150]byte
	size := len(header)
	if ext == ".gba" {
		size = 192
	}
	_, err := io.ReadFull(r, header[:size])
	return err == nil && looksLikeROM(header[:size], ext)
}

func stageDownload(c *http.Client, address, dir string) (*os.File, int64, error) {
	res, err := fetch(c, address, nil, "")
	if err != nil {
		return nil, 0, err
	}
	defer res.Body.Close()
	if res.ContentLength > maxArchive {
		return nil, 0, errors.New("This download is too large for Pocket Boy")
	}
	file, err := os.CreateTemp(dir, ".q2-boy-download-*")
	if err != nil {
		return nil, 0, errors.New("Could not write to the SD card")
	}
	// An unlinked work file is reclaimed even when firmware closes the app.
	if err := os.Remove(file.Name()); err != nil {
		defer os.Remove(file.Name())
	}
	size, err := io.Copy(file, io.LimitReader(res.Body, maxArchive+1))
	if err != nil || size > maxArchive {
		file.Close()
		return nil, 0, errors.New("Download incomplete or too large")
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		file.Close()
		return nil, 0, errors.New("Could not read the staged download")
	}
	return file, size, nil
}

func titleStem(title string, id int64) string {
	stem := strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune(`<>:"/\|?*`, r) {
			return ' '
		}
		return r
	}, title)
	ext := strings.ToLower(filepath.Ext(stem))
	if supported(ext) {
		stem = strings.TrimSuffix(stem, filepath.Ext(stem))
	}
	stem = strings.Trim(clean(stem, 112), ". ")
	if stem == "" {
		stem = "itch.io game"
	}
	return fmt.Sprintf("%s [itch %d]", stem, id)
}

// Pocket Boy serializes imports. Link atomically where supported, then use
// rename on SD filesystems that lack hard links, with an existence check.
func install(dir, name string, data io.Reader, limit int64) error {
	dst := filepath.Join(dir, name)
	if _, err := os.Lstat(dst); !os.IsNotExist(err) {
		if err == nil {
			return errExists
		}
		return err
	}
	f, err := os.CreateTemp(dir, ".q2-boy-itch-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	size, err := io.Copy(f, io.LimitReader(data, limit+1))
	if err == nil && size > limit {
		err = errors.New("File exceeds its size limit")
	}
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if err = os.Link(f.Name(), dst); err == nil {
		return nil
	} else if os.IsExist(err) {
		return errExists
	}
	if _, err = os.Lstat(dst); !os.IsNotExist(err) {
		return errExists
	}
	return os.Rename(f.Name(), dst)
}

func cover(c *http.Client, address string) ([]byte, error) {
	if address == "" {
		return nil, errVisit
	}
	res, err := fetch(c, address, nil, "")
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	raw, err := limited(res.Body, 3<<20)
	if err != nil {
		return nil, err
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(raw))
	if err != nil || cfg.Width < 1 || cfg.Height < 1 || cfg.Width > 1024 || cfg.Height > 1024 {
		return nil, errVisit
	}
	img, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return nil, err
	}
	var pngFile bytes.Buffer
	err = png.Encode(&pngFile, img)
	return pngFile.Bytes(), err
}

func uploadKind(name string) string {
	if strings.EqualFold(path.Ext(name), ".zip") {
		return ".zip"
	}
	if system := uploadConsole(name); system != "" {
		return "." + system
	}
	return ""
}

func download(c *http.Client, base, dirs, preferred string, output io.Writer) error {
	data, err := metadata(c, base)
	if err != nil {
		return err
	}
	if label := priceLabel(data.Price); label != "Free" && label != "Free / name your price" {
		return errVisit
	}
	stem := titleStem(data.Title, data.ID)
	for _, system := range []string{"gb", "gbc", "gba"} {
		dir := filepath.Join(dirs, system)
		found, err := os.Stat(filepath.Join(dir, stem+"."+system))
		if err == nil && found.Mode().IsRegular() {
			if _, err := os.Stat(filepath.Join(dir, stem+".png")); err == nil {
				return errExists
			}
			if saveCover(c, data.Cover, dir, stem) {
				fmt.Fprintln(output, "Cover added to your existing ROM")
				return nil
			}
			return errors.New("ROM already installed. Cover art was unavailable")
		}
	}
	files, signed, csrf, key, err := guestUploads(c, base)
	if err != nil {
		return err
	}
	priority := func(file upload) int {
		switch uploadKind(file.name) {
		case preferred:
			return 0
		case ".zip":
			return 1
		default:
			return 2
		}
	}
	sort.SliceStable(files, func(i, j int) bool { return priority(files[i]) < priority(files[j]) })
	var dir string
	var blocked bool
	attempts := 0
	for _, file := range files {
		kind := uploadKind(file.name)
		if file.id == "" || (!supported(kind) && kind != ".zip") {
			continue
		}
		attempts++
		if attempts > 6 {
			break
		}
		cdn, err := postURL(c, base+"/file/"+file.id, signed,
			url.Values{"csrf_token": {csrf}, "download_key_id": {key}})
		if errors.Is(err, errVisit) {
			blocked = true
			continue
		}
		if err != nil {
			return err
		}
		file, size, err := stageDownload(c, cdn, dirs)
		if err != nil {
			return err
		}
		rom, ext, err := romReader(file, size, kind, preferred)
		if err != nil {
			file.Close()
			continue
		}
		dir = filepath.Join(dirs, ext[1:])
		err = install(dir, stem+ext, rom, maxROM)
		rom.Close()
		file.Close()
		if err != nil {
			if errors.Is(err, errExists) {
				return err
			}
			return errors.New("Could not save the ROM to the SD card")
		}
		break
	}
	if dir == "" {
		if blocked {
			return errVisit
		}
		return errNoROM
	}
	if saveCover(c, data.Cover, dir, stem) {
		fmt.Fprintln(output, "ROM and cover added to your library")
	} else {
		fmt.Fprintln(output, "ROM added. Cover art was unavailable")
	}
	return nil
}

func saveCover(c *http.Client, address, dir, stem string) bool {
	art, err := cover(c, address)
	if err != nil {
		return false
	}
	err = install(dir, stem+".png", bytes.NewReader(art), 8<<20)
	return err == nil || errors.Is(err, errExists)
}

func run(args []string, output io.Writer) error {
	c := newClient()
	if len(args) == 7 && args[0] == "list" {
		n, _ := strconv.Atoi(args[5])
		return list(c, args[1], args[2], args[3], args[4], n, args[6], output)
	}
	if len(args) < 2 {
		return errVisit
	}
	u, ok := project(args[1])
	if !ok {
		return errVisit
	}
	base := strings.TrimRight(u.String(), "/")
	switch args[0] {
	case "info":
		data, err := metadata(c, base)
		if err != nil {
			return err
		}
		fmt.Fprintf(output, "%s\t%s\n", priceLabel(data.Price), clean(data.Suggested, 20))
		return nil
	case "download":
		if len(args) == 4 && supported("."+args[3]) {
			return download(c, base, args[2], "."+args[3], output)
		}
	case "qr":
		qr, err := qrcode.New(base, qrcode.Medium)
		if err != nil {
			return errVisit
		}
		for _, row := range qr.Bitmap() {
			for _, bit := range row {
				if bit {
					fmt.Fprint(output, "1")
				} else {
					fmt.Fprint(output, "0")
				}
			}
			fmt.Fprintln(output)
		}
		return nil
	}
	return errVisit
}

func main() {
	if os.Getenv("SSL_CERT_FILE") == "" {
		if exe, err := os.Executable(); err == nil {
			certs := filepath.Join(filepath.Dir(exe), "itch-ca.pem")
			if _, err := os.Stat(certs); err == nil {
				_ = os.Setenv("SSL_CERT_FILE", certs)
			}
		}
	}
	if err := run(os.Args[1:], os.Stdout); err != nil {
		fmt.Fprintln(os.Stdout, err)
		os.Exit(1)
	}
}
