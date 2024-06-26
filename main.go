package main

import (
	"bufio"
	"errors"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"
)

const (
	CanvasWidth  = 800
	CanvasHeight = 400
	CanvasDiag   = 894.4271909999159 // run it yourself
	MaxGeoms     = 1000
)

// WKT types
const (
	PointType = iota
	SegmentType
)

// Error codes
var (
	ErrParseNumber        = errors.New("failed to parse number")
	ErrUnsupportedWKT     = errors.New("unsupported WKT")
	ErrRenderInvalidCoord = errors.New("invalid coordinate for rendering")
)

type Point struct {
	x, y float64
}

type Segment struct {
	a, b Point
}

type Geom struct {
	geomType int
	point    Point
	segment  Segment
	roundR   float64
}

func parseNumber(str string, cursor *int) (float64, error) {
	start := *cursor
	for *cursor < len(str) && (str[*cursor] >= '0' && str[*cursor] <= '9' || str[*cursor] == '.' || str[*cursor] == '-') {
		*cursor++
	}
	if *cursor == start {
		return 0, ErrParseNumber
	}
	numStr := str[start:*cursor]
	num, err := strconv.ParseFloat(numStr, 64)
	if err != nil {
		return 0, err
	}
	return num, nil
}

func parsePoint(line string, cursor *int) (Point, error) {
	var point Point
	*cursor += 6 // Skip POINT(
	x, err := parseNumber(line, cursor)
	if err != nil {
		return point, err
	}
	point.x = x * CanvasWidth
	*cursor++ // Skip the separator space
	y, err := parseNumber(line, cursor)
	if err != nil {
		return point, err
	}
	point.y = y * CanvasHeight
	*cursor++ // Skip the )
	return point, nil
}

func parseSegment(line string, cursor *int) (Segment, error) {
	var segment Segment
	*cursor += 8 // Skip SEGMENT(
	pointA, err := parsePoint(line, cursor)
	if err != nil {
		return segment, err
	}
	*cursor++ // Skip the separator space
	pointB, err := parsePoint(line, cursor)
	if err != nil {
		return segment, err
	}
	segment.a = pointA
	segment.b = pointB
	*cursor++ // Skip the )
	return segment, nil
}

func parseRound(line string, cursor *int) (Geom, error) {
	var geo Geom
	*cursor += 6 // Skip ROUND(
	segment, err := parseSegment(line, cursor)
	if err != nil {
		return geo, err
	}
	*cursor++ // Skip the separator space
	r, err := parseNumber(line, cursor)
	if err != nil {
		return geo, err
	}
	*cursor++ // Skip the )
	geo.geomType = SegmentType
	geo.segment = segment
	geo.roundR = r * CanvasDiag
	return geo, nil
}

func parseLine(line string) (Geom, error) {
	var geo Geom
	cursor := 0
	switch {
	case strings.HasPrefix(line, "POINT"):
		point, err := parsePoint(line, &cursor)
		if err != nil {
			return geo, err
		}
		geo.geomType = PointType
		geo.point = point
	case strings.HasPrefix(line, "SEGMENT"):
		segment, err := parseSegment(line, &cursor)
		if err != nil {
			return geo, err
		}
		geo.geomType = SegmentType
		geo.segment = segment
	case strings.HasPrefix(line, "ROUND"):
		round, err := parseRound(line, &cursor)
		if err != nil {
			return geo, err
		}
		geo = round
	default:
		return geo, ErrUnsupportedWKT
	}
	return geo, nil
}

func createBitmapFileHeader(height, stride int) []byte {
	fileSize := 14 + 40 + stride*height
	return []byte{
		'B', 'M',
		byte(fileSize), byte(fileSize >> 8), byte(fileSize >> 16), byte(fileSize >> 24),
		0, 0, 0, 0,
		54, 0, 0, 0,
	}
}

func createBitmapInfoHeader(height, width int) []byte {
	return []byte{
		40, 0, 0, 0, // Header size
		byte(width), byte(width >> 8), byte(width >> 16), byte(width >> 24), // Width
		byte(height), byte(height >> 8), byte(height >> 16), byte(height >> 24), // Height
		1, 0, // Planes
		24, 0, // Bits per pixel
		0, 0, 0, 0, // Compression (no compression)
		0, 0, 0, 0, // Image size (no compression)
		0, 0, 0, 0, // X pixels per meter (unspecified)
		0, 0, 0, 0, // Y pixels per meter (unspecified)
		0, 0, 0, 0, // Total colors (color table not used)
		0, 0, 0, 0, // Important colors (generally ignored)
	}
}

func writeBitmap(geoms []Geom, size int, imageFileName string) error {
	widthInBytes := CanvasWidth * 3
	paddingSize := (4 - (widthInBytes % 4)) % 4
	stride := widthInBytes + paddingSize

	file, err := os.Create(imageFileName)
	if err != nil {
		return err
	}
	defer file.Close()

	fileHeader := createBitmapFileHeader(CanvasHeight, stride)
	_, err = file.Write(fileHeader)
	if err != nil {
		return err
	}

	infoHeader := createBitmapInfoHeader(CanvasHeight, CanvasWidth)
	_, err = file.Write(infoHeader)
	if err != nil {
		return err
	}

	for y := 0; y < CanvasHeight; y++ {
		for x := 0; x < CanvasWidth; x++ {
			pixel := [3]byte{0, 0, 0}
			sdRender(geoms, float64(x), float64(y), &pixel)
			_, err := file.Write(pixel[:])
			if err != nil {
				return err
			}
		}
		_, err := file.Write(make([]byte, paddingSize))
		if err != nil {
			return err
		}
	}
	return nil
}

/* SDF */
// length function
func length(p Point) float64 {
	return math.Sqrt(p.x*p.x + p.y*p.y)
}

// sub function
func sub(a, b Point) Point {
	return Point{a.x - b.x, a.y - b.y}
}

// mul function
func mul(a Point, s float64) Point {
	return Point{a.x * s, a.y * s}
}

// dot function
func dot(a, b Point) float64 {
	return a.x*b.x + a.y*b.y
}

// min function
func min(a, b float64) float64 {
	if a < b {
		return a
	}
	return b
}

// max function
func max(a, b float64) float64 {
	if a > b {
		return a
	}
	return b
}

// clamp function
func clamp(v, vmin, vmax float64) float64 {
	return max(vmin, min(v, vmax))
}

// opRound function
func opRound(d, r float64) float64 {
	return d - r
}

// sdPoint function
func sdPoint(p, a Point) float64 {
	return length(sub(p, a))
}

// sdSegment function
func sdSegment(p, a, b Point) float64 {
	pa := sub(p, a)
	ba := sub(b, a)
	h := clamp(dot(pa, ba)/dot(ba, ba), 0.0, 1.0)
	return length(sub(pa, mul(ba, h)))
}

// sdRender function
func sdRender(geoms []Geom, x, y float64, pixel *[3]byte) {
	d := length(Point{CanvasWidth, CanvasHeight})
	p := Point{x, y}
	for i := 0; i < len(geoms); i++ {
		switch geoms[i].geomType {
		case PointType:
			d = min(d, opRound(sdPoint(p, geoms[i].point), geoms[i].roundR))
		case SegmentType:
			d = min(d, opRound(sdSegment(p, geoms[i].segment.a, geoms[i].segment.b), geoms[i].roundR))
		}

		if d < 0 {
			pixel[0] = byte((i * 2) % 255)
			pixel[1] = byte((100 + i) % 255)
			pixel[2] = byte((50 / (i + 1)) % 255)
			break
		}
	}
}

/* === */

func main() {
	geoms := make([]Geom, 0, MaxGeoms)

	file, err := os.Open("segments.wkt")
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		geo, err := parseLine(line)
		if err != nil {
			fmt.Println(err)
			continue
		}
		geoms = append(geoms, geo)
		if len(geoms) >= MaxGeoms {
			break
		}
	}

	err = writeBitmap(geoms, len(geoms), "canvas.bmp")
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
