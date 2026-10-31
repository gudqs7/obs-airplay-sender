package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"doubletake/internal/airplay"
)

const (
	ipcMagic       = uint32(0x4f415043)
	ipcHeaderBytes = 24

	ipcStart    = uint16(1)
	ipcVideo    = uint16(2)
	ipcAudio    = uint16(3)
	ipcStop     = uint16(4)
	ipcClock    = uint16(5)
	ipcDiscover = uint16(6)
	ipcResponse = uint16(100)
)

type ipcHeader struct {
	Magic    uint32
	Type     uint16
	Flags    uint16
	Length   uint32
	Reserved uint32
	PTS      uint64
}

type startRequest struct {
	Target           string `json:"target"`
	TargetIP         string `json:"targetIP,omitempty"`
	TargetPort       int    `json:"targetPort,omitempty"`
	Password         string `json:"password,omitempty"`
	FPS              int    `json:"fps"`
	Bitrate          int    `json:"bitrate"`
	NoAudio          bool   `json:"noAudio,omitempty"`
	DropAudioPackets bool   `json:"dropAudioPackets,omitempty"`
	NoEncrypt        bool   `json:"noEncrypt,omitempty"`
	FairPlayKeyMode  string `json:"fairplayKeyMode,omitempty"`
}

type startResponse struct {
	OK         bool   `json:"ok"`
	Message    string `json:"message,omitempty"`
	Width      int    `json:"width,omitempty"`
	Height     int    `json:"height,omitempty"`
	AudioCodec int    `json:"audioCodec,omitempty"`
}

type deviceInfo struct {
	Name string `json:"name"`
	IP   string `json:"ip"`
	Port int    `json:"port"`
}

type discoverResponse struct {
	OK      bool         `json:"ok"`
	Message string       `json:"message,omitempty"`
	Devices []deviceInfo `json:"devices,omitempty"`
}

type discoverRequest struct {
	Preferred string `json:"preferred,omitempty"`
}

const (
	discoveryRefreshInterval = 5 * time.Second
	discoveryStaleAfter      = 12 * time.Second
	discoveryWaitLimit       = 900 * time.Millisecond
	discoverySettleWindow    = 150 * time.Millisecond
)

type cachedDevice struct {
	device airplay.AirPlayDevice
	seen   time.Time
}

// discoveryCache keeps the latest mDNS advertisements hot while the bridge is
// idle. A scan can then return an already-known receiver without paying another
// full browse interval, while a newly started receiver is still picked up by
// the short wait in waitFor.
type discoveryCache struct {
	ctx     context.Context
	cancel  context.CancelFunc
	mu      sync.Mutex
	devices map[string]cachedDevice
	changed chan struct{}
}

func newDiscoveryCache(parent context.Context) *discoveryCache {
	ctx, cancel := context.WithCancel(parent)
	return &discoveryCache{
		ctx:     ctx,
		cancel:  cancel,
		devices: make(map[string]cachedDevice),
		changed: make(chan struct{}),
	}
}

func (c *discoveryCache) start() {
	go c.run()
}

func (c *discoveryCache) stop() {
	if c != nil {
		c.cancel()
	}
}

func (c *discoveryCache) run() {
	for c.ctx.Err() == nil {
		browseCtx, cancel := context.WithTimeout(c.ctx, discoveryRefreshInterval)
		err := airplay.BrowseAirPlayDevices(browseCtx, c.observe)
		cancel()
		if c.ctx.Err() != nil {
			return
		}
		if err != nil && !errors.Is(err, context.DeadlineExceeded) &&
			!errors.Is(err, context.Canceled) {
			log.Printf("mDNS cache browse error: %v", err)
		}
		timer := time.NewTimer(100 * time.Millisecond)
		select {
		case <-c.ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

func (c *discoveryCache) observe(device airplay.AirPlayDevice) {
	now := time.Now()
	key := device.Name + "\x00" + device.IP + "\x00" + fmt.Sprint(device.Port)
	c.mu.Lock()
	defer c.mu.Unlock()
	c.devices[key] = cachedDevice{device: device, seen: now}
	close(c.changed)
	c.changed = make(chan struct{})
}

func deviceNameMatches(device airplay.AirPlayDevice, target string) bool {
	target = strings.TrimSpace(target)
	return target == "" || device.Name == target || strings.Contains(device.Name, target)
}

func (c *discoveryCache) pruneLocked(now time.Time) {
	for key, cached := range c.devices {
		if now.Sub(cached.seen) > discoveryStaleAfter {
			delete(c.devices, key)
		}
	}
}

func (c *discoveryCache) snapshotLocked(now time.Time) []airplay.AirPlayDevice {
	c.pruneLocked(now)
	devices := make([]airplay.AirPlayDevice, 0, len(c.devices))
	for _, cached := range c.devices {
		devices = append(devices, cached.device)
	}
	return devices
}

func (c *discoveryCache) lookupEndpoint(ip string, port int, target string) (airplay.AirPlayDevice, bool) {
	if c == nil {
		return airplay.AirPlayDevice{}, false
	}
	now := time.Now()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, cached := range c.devices {
		if now.Sub(cached.seen) > discoveryStaleAfter {
			continue
		}
		if cached.device.IP == ip && cached.device.Port == port &&
			deviceNameMatches(cached.device, target) {
			return cached.device, true
		}
	}
	return airplay.AirPlayDevice{}, false
}

func (c *discoveryCache) removeEndpoint(ip string, port int) {
	if c == nil {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for key, cached := range c.devices {
		if cached.device.IP == ip && cached.device.Port == port {
			delete(c.devices, key)
		}
	}
}

func (c *discoveryCache) waitFor(preferred string, wait, settle time.Duration) ([]airplay.AirPlayDevice, bool) {
	if c == nil {
		return nil, false
	}
	if wait <= 0 {
		wait = discoveryWaitLimit
	}
	deadline := time.Now().Add(wait)
	var firstSeen time.Time
	for {
		now := time.Now()
		c.mu.Lock()
		devices := c.snapshotLocked(now)
		changed := c.changed
		c.mu.Unlock()

		for _, device := range devices {
			if strings.TrimSpace(preferred) != "" && deviceNameMatches(device, preferred) {
				return devices, true
			}
		}
		if strings.TrimSpace(preferred) == "" && len(devices) > 0 {
			return devices, true
		}
		if len(devices) > 0 && firstSeen.IsZero() {
			firstSeen = now
		}
		if !firstSeen.IsZero() && settle > 0 && strings.TrimSpace(preferred) == "" {
			if now.Sub(firstSeen) >= settle {
				return devices, false
			}
		}
		if !now.Before(deadline) {
			return devices, false
		}

		delay := time.Until(deadline)
		if !firstSeen.IsZero() && settle > 0 && strings.TrimSpace(preferred) == "" {
			if settleDelay := settle - now.Sub(firstSeen); settleDelay < delay {
				delay = settleDelay
			}
		}
		timer := time.NewTimer(delay)
		select {
		case <-c.ctx.Done():
			timer.Stop()
			return devices, false
		case <-changed:
			timer.Stop()
		case <-timer.C:
		}
	}
}

type videoItem struct {
	unit airplay.VideoAccessUnit
}

type videoSource struct {
	ctx context.Context
	ch  chan videoItem
}

func (s *videoSource) ReadVideoAccessUnit() (airplay.VideoAccessUnit, error) {
	select {
	case <-s.ctx.Done():
		return airplay.VideoAccessUnit{}, io.EOF
	case item := <-s.ch:
		return item.unit, nil
	}
}

type audioChunk struct {
	data []byte
	pts  time.Time
}

type audioSource struct {
	ctx    context.Context
	ch     chan audioChunk
	mu     sync.Mutex
	buf    []byte
	bufPTS time.Time
}

func (s *audioSource) ReadPCMFrame(dst []byte) (time.Time, error) {
	for len(s.buf) < len(dst) {
		select {
		case <-s.ctx.Done():
			return time.Time{}, io.EOF
		case chunk := <-s.ch:
			if len(s.buf) == 0 {
				s.bufPTS = chunk.pts
			}
			s.buf = append(s.buf, chunk.data...)
		}
	}

	pts := s.bufPTS
	copy(dst, s.buf[:len(dst)])
	s.buf = s.buf[len(dst):]
	s.bufPTS = pts.Add(time.Duration(len(dst)/4) * time.Second / 44100)
	return pts, nil
}

type streamSession struct {
	mu         sync.RWMutex
	client     *airplay.AirPlayClient
	mirror     *airplay.MirrorSession
	cancel     context.CancelFunc
	video      *videoSource
	audio      *audioSource
	done       chan struct{}
	closeOnce  sync.Once
	obsBase    uint64
	goBase     time.Time
	haveClock  bool
	width      int
	height     int
	startedAt  time.Time
	firstVideo sync.Once
	firstAudio sync.Once
}

func (s *streamSession) setClock(obsNS uint64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.haveClock {
		return
	}
	s.obsBase = obsNS
	s.goBase = time.Now()
	s.haveClock = true
}

func (s *streamSession) mapTime(obsNS uint64) time.Time {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.haveClock {
		s.obsBase = obsNS
		s.goBase = time.Now()
		s.haveClock = true
	}
	if obsNS >= s.obsBase {
		return s.goBase.Add(time.Duration(obsNS - s.obsBase))
	}
	return s.goBase.Add(-time.Duration(s.obsBase - obsNS))
}

func (s *streamSession) pushVideo(obsNS uint64, data []byte) bool {
	annexB, err := normalizeAnnexB(data)
	if err != nil {
		log.Printf("video packet rejected: %v", err)
		return false
	}
	select {
	case s.video.ch <- videoItem{unit: airplay.VideoAccessUnit{
		AnnexB: annexB,
		PTS:    s.mapTime(obsNS),
	}}:
		s.firstVideo.Do(func() {
			log.Printf("[TIMING] start: first OBS video packet +%s",
				time.Since(s.startedAt).Round(time.Millisecond))
		})
		return true
	default:
		return false
	}
}

func (s *streamSession) pushAudio(obsNS uint64, data []byte) bool {
	if len(data) == 0 || len(data)%4 != 0 {
		return false
	}
	select {
	case s.audio.ch <- audioChunk{data: data, pts: s.mapTime(obsNS)}:
		s.firstAudio.Do(func() {
			log.Printf("[TIMING] start: first OBS audio packet +%s",
				time.Since(s.startedAt).Round(time.Millisecond))
		})
		return true
	default:
		return false
	}
}

func (s *streamSession) stop() {
	s.closeOnce.Do(func() {
		s.cancel()
		if s.mirror != nil {
			_ = s.mirror.Close()
		}
		if s.client != nil {
			_ = s.client.Close()
		}
		select {
		case <-s.done:
		case <-time.After(3 * time.Second):
		}
	})
}

func startSession(parent context.Context, request startRequest, cache *discoveryCache) (*streamSession, error) {
	startedAt := time.Now()
	stage := func(name string) {
		log.Printf("[TIMING] start: %s +%s", name, time.Since(startedAt).Round(time.Millisecond))
	}
	stage("request accepted")
	if request.FPS == 0 {
		request.FPS = 60
	}
	if request.Bitrate == 0 {
		request.Bitrate = 12000
	}
	if request.FPS < 15 || request.FPS > 120 {
		return nil, fmt.Errorf("invalid FPS %d", request.FPS)
	}
	if request.Bitrate < 2000 || request.Bitrate > 30000 {
		return nil, fmt.Errorf("invalid bitrate %d kbps", request.Bitrate)
	}

	ctx, cancel := context.WithCancel(parent)
	client, info, err := connectReceiver(ctx, request, cache)
	if err != nil {
		cancel()
		return nil, err
	}
	stage("receiver connected and /info loaded")

	if err := prepareReceiver(ctx, client, info, request.Password); err != nil {
		_ = client.Close()
		cancel()
		return nil, err
	}
	stage("pair-verify and FairPlay ready")

	video := &videoSource{ctx: ctx, ch: make(chan videoItem, 90)}
	audioChannel := make(chan audioChunk, 256)
	audio := &audioSource{ctx: ctx, ch: audioChannel}

	width, height := 1920, 1080
	cfg := airplay.StreamConfig{
		FPS:        request.FPS,
		Bitrate:    request.Bitrate,
		VideoCodec: airplay.VideoCodecH264,
		NoAudio:    request.NoAudio,
		NoEncrypt:  request.NoEncrypt,
	}
	mirror, err := client.SetupMirrorWithVideoPreparation(ctx, cfg, func(w, h int) error {
		if w > 0 && h > 0 {
			width, height = w, h
		}
		return nil
	})
	if err != nil {
		_ = client.Close()
		cancel()
		return nil, fmt.Errorf("mirror setup: %w", err)
	}
	stage("AirPlay SETUP and RECORD ready")
	if w, h := mirror.VideoDimensions(); w > 0 && h > 0 {
		width, height = w, h
	}
	width &^= 1
	height &^= 1
	if width <= 0 || height <= 0 {
		_ = mirror.Close()
		_ = client.Close()
		cancel()
		return nil, errors.New("receiver returned an invalid mirror canvas")
	}

	session := &streamSession{
		client:    client,
		mirror:    mirror,
		cancel:    cancel,
		video:     video,
		audio:     audio,
		done:      make(chan struct{}, 2),
		width:     width,
		height:    height,
		startedAt: startedAt,
	}

	var workers sync.WaitGroup
	workers.Add(2)
	go func() {
		defer workers.Done()
		err := mirror.StreamFrames(ctx, airplay.NewExternalVideoCapture(video), 0)
		if err != nil && ctx.Err() == nil {
			log.Printf("video stream ended: %v", err)
			session.stop()
		}
	}()
	go func() {
		defer workers.Done()
		if request.NoAudio || request.DropAudioPackets || !mirror.HasAudio() {
			return
		}
		capture := airplay.NewExternalAudioCapture(mirror.AudioCodec(), audio)
		defer capture.Stop()
		err := mirror.StreamAudio(ctx, capture, mirror.AudioStream())
		if err != nil && ctx.Err() == nil {
			log.Printf("audio stream ended: %v", err)
			session.stop()
		}
	}()
	go func() {
		workers.Wait()
		session.done <- struct{}{}
	}()

	log.Printf("airplay session ready: target=%q canvas=%dx%d audioCodec=%d fairplayKeyMode=%q",
		request.Target, width, height, mirror.AudioCodec(), request.FairPlayKeyMode)
	stage("stream workers started")
	return session, nil
}

func connectReceiver(ctx context.Context, request startRequest, cache *discoveryCache) (*airplay.AirPlayClient, *airplay.ReceiverInfo, error) {
	startedAt := time.Now()
	target := strings.TrimSpace(request.Target)
	if target == "" {
		target = "抖音直播伴侣"
	}

	targetIP := strings.TrimSpace(request.TargetIP)
	targetPort := request.TargetPort
	if targetIP == "" && net.ParseIP(target) != nil {
		targetIP = target
	}
	if targetPort == 0 {
		targetPort = 7000
	}

	connectDevice := func(device *airplay.AirPlayDevice) (*airplay.AirPlayClient, *airplay.ReceiverInfo, error) {
		connectCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		defer cancel()
		client := airplay.NewAirPlayClientForDevice(*device)
		client.FairPlayKeyMode = request.FairPlayKeyMode
		client.SetPassword(request.Password)
		if err := client.Connect(connectCtx); err != nil {
			return nil, nil, fmt.Errorf("connect to AirPlay receiver: %w", err)
		}
		info, err := client.GetInfo()
		if err != nil {
			_ = client.Close()
			return nil, nil, fmt.Errorf("read AirPlay receiver info: %w", err)
		}
		return client, info, nil
	}

	if targetIP != "" {
		device := airplay.AirPlayDevice{Name: target, IP: targetIP, Port: targetPort}
		source := "selected endpoint"
		if cached, ok := cache.lookupEndpoint(targetIP, targetPort, target); ok {
			device = cached
			source = "cached mDNS advertisement"
		}
		log.Printf("receiver connect: %s %s:%d", source, targetIP, targetPort)
		client, info, err := connectDevice(&device)
		if err == nil {
			return client, info, nil
		}
		log.Printf("receiver endpoint failed, refreshing mDNS by name: %v", err)
		cache.removeEndpoint(targetIP, targetPort)
	}

	var device *airplay.AirPlayDevice
	if cache != nil {
		devices, matched := cache.waitFor(target, discoveryWaitLimit, discoverySettleWindow)
		log.Printf("receiver discovery: cached=%d matched=%t elapsed=%s",
			len(devices), matched, time.Since(startedAt).Round(time.Millisecond))
		if matched {
			for index := range devices {
				if deviceNameMatches(devices[index], target) {
					device = &devices[index]
					break
				}
			}
		}
	}
	if device == nil {
		return nil, nil, fmt.Errorf("AirPlay receiver %q was not found on the LAN", target)
	}
	log.Printf("receiver connect: refreshed mDNS advertisement %s:%d", device.IP, device.Port)
	client, info, err := connectDevice(device)
	if err != nil {
		return nil, nil, err
	}
	return client, info, nil
}

func prepareReceiver(ctx context.Context, client *airplay.AirPlayClient, info *airplay.ReceiverInfo, password string) error {
	if info.RequiresPassword() && password == "" {
		return errors.New("the receiver requires a password")
	}
	if info.RequiredPairingCredential() == airplay.PairingCredentialPIN && password == "" {
		return errors.New("the receiver requires its on-screen pairing PIN")
	}

	client.SetPassword(password)
	pairValue := ""
	if info.RequiredPairingCredential() == airplay.PairingCredentialPIN {
		pairValue = password
	}
	if err := client.Pair(ctx, pairValue); err != nil {
		if info.RequiresPassword() {
			return fmt.Errorf("pair with receiver: %w", err)
		}
		return fmt.Errorf("start AirPlay session: %w", err)
	}

	if client.FpEkey == nil {
		if err := client.FairPlaySetup(ctx); err != nil && !errors.Is(err, airplay.ErrFairPlayUnsupported) {
			return fmt.Errorf("FairPlay setup: %w", err)
		}
	}
	return nil
}

func normalizeAnnexB(data []byte) ([]byte, error) {
	if len(data) == 0 {
		return nil, errors.New("empty access unit")
	}
	if (len(data) >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
		(len(data) >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
		return append([]byte(nil), data...), nil
	}

	out := make([]byte, 0, len(data)+32)
	for offset := 0; offset < len(data); {
		if len(data)-offset < 4 {
			return nil, errors.New("truncated length-prefixed H.264 packet")
		}
		n := int(binary.BigEndian.Uint32(data[offset : offset+4]))
		offset += 4
		if n <= 0 || n > len(data)-offset {
			return nil, fmt.Errorf("invalid H.264 NAL length %d", n)
		}
		out = append(out, 0, 0, 0, 1)
		out = append(out, data[offset:offset+n]...)
		offset += n
	}
	return out, nil
}

func readHeader(reader io.Reader) (ipcHeader, error) {
	var raw [ipcHeaderBytes]byte
	if _, err := io.ReadFull(reader, raw[:]); err != nil {
		return ipcHeader{}, err
	}
	header := ipcHeader{
		Magic:    binary.LittleEndian.Uint32(raw[0:4]),
		Type:     binary.LittleEndian.Uint16(raw[4:6]),
		Flags:    binary.LittleEndian.Uint16(raw[6:8]),
		Length:   binary.LittleEndian.Uint32(raw[8:12]),
		Reserved: binary.LittleEndian.Uint32(raw[12:16]),
		PTS:      binary.LittleEndian.Uint64(raw[16:24]),
	}
	if header.Magic != ipcMagic {
		return ipcHeader{}, errors.New("invalid IPC magic")
	}
	if header.Length > 32<<20 {
		return ipcHeader{}, fmt.Errorf("IPC payload too large: %d", header.Length)
	}
	return header, nil
}

func writePacket(conn net.Conn, header ipcHeader, payload []byte) error {
	header.Magic = ipcMagic
	header.Length = uint32(len(payload))
	var raw [ipcHeaderBytes]byte
	binary.LittleEndian.PutUint32(raw[0:4], header.Magic)
	binary.LittleEndian.PutUint16(raw[4:6], header.Type)
	binary.LittleEndian.PutUint16(raw[6:8], header.Flags)
	binary.LittleEndian.PutUint32(raw[8:12], header.Length)
	binary.LittleEndian.PutUint32(raw[12:16], header.Reserved)
	binary.LittleEndian.PutUint64(raw[16:24], header.PTS)
	if _, err := conn.Write(raw[:]); err != nil {
		return err
	}
	if len(payload) > 0 {
		_, err := conn.Write(payload)
		return err
	}
	return nil
}

func respond(conn net.Conn, response startResponse) error {
	payload, _ := json.Marshal(response)
	return writePacket(conn, ipcHeader{Type: ipcResponse}, payload)
}

func respondDevices(conn net.Conn, response discoverResponse) error {
	payload, _ := json.Marshal(response)
	return writePacket(conn, ipcHeader{Type: ipcResponse}, payload)
}

func discoverDevices(ctx context.Context, cache *discoveryCache, preferred string) ([]deviceInfo, error) {
	startedAt := time.Now()
	var found []airplay.AirPlayDevice
	if cache != nil {
		var matched bool
		found, matched = cache.waitFor(preferred, discoveryWaitLimit, discoverySettleWindow)
		log.Printf("discovery request: preferred=%q cached=%d matched=%t elapsed=%s",
			preferred, len(found), matched, time.Since(startedAt).Round(time.Millisecond))
	} else {
		discoverCtx, cancel := context.WithTimeout(ctx, discoveryWaitLimit)
		defer cancel()
		var err error
		found, err = airplay.DiscoverAirPlayDevices(discoverCtx)
		if err != nil {
			return nil, err
		}
		log.Printf("discovery request: fallback cached=%d elapsed=%s",
			len(found), time.Since(startedAt).Round(time.Millisecond))
	}
	devices := make([]deviceInfo, 0, len(found))
	seen := make(map[string]bool, len(found))
	for _, device := range found {
		key := device.Name + "\x00" + device.IP + "\x00" + fmt.Sprint(device.Port)
		if seen[key] {
			continue
		}
		seen[key] = true
		devices = append(devices, deviceInfo{
			Name: device.Name,
			IP:   device.IP,
			Port: device.Port,
		})
	}
	return devices, nil
}

func handleConnection(conn net.Conn, cache *discoveryCache) {
	defer conn.Close()
	reader := bufio.NewReaderSize(conn, 1<<20)
	var session *streamSession
	defer func() {
		if session != nil {
			session.stop()
		}
	}()

	for {
		header, err := readHeader(reader)
		if err != nil {
			if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				log.Printf("IPC read ended: %v", err)
			}
			return
		}
		payload := make([]byte, header.Length)
		if len(payload) > 0 {
			if _, err := io.ReadFull(reader, payload); err != nil {
				log.Printf("IPC payload read failed: %v", err)
				return
			}
		}

		switch header.Type {
		case ipcDiscover:
			var request discoverRequest
			if len(payload) > 0 {
				if err := json.Unmarshal(payload, &request); err != nil {
					_ = respondDevices(conn, discoverResponse{OK: false, Message: "invalid discovery request"})
					continue
				}
			}
			devices, err := discoverDevices(context.Background(), cache, request.Preferred)
			if err != nil {
				_ = respondDevices(conn, discoverResponse{OK: false, Message: err.Error()})
				continue
			}
			_ = respondDevices(conn, discoverResponse{OK: true, Devices: devices})
		case ipcStart:
			if session != nil {
				_ = respond(conn, startResponse{OK: false, Message: "an AirPlay session is already active"})
				continue
			}
			var request startRequest
			if err := json.Unmarshal(payload, &request); err != nil {
				_ = respond(conn, startResponse{OK: false, Message: "invalid start request"})
				continue
			}
			started, err := startSession(context.Background(), request, cache)
			if err != nil {
				log.Printf("start failed: %v", err)
				_ = respond(conn, startResponse{OK: false, Message: err.Error()})
				continue
			}
			session = started
			_ = respond(conn, startResponse{
				OK:         true,
				Width:      session.width,
				Height:     session.height,
				AudioCodec: int(session.mirror.AudioCodec()),
			})
		case ipcClock:
			if session != nil {
				session.setClock(header.PTS)
			}
		case ipcVideo:
			if session != nil {
				session.pushVideo(header.PTS, payload)
			}
		case ipcAudio:
			if session != nil {
				session.pushAudio(header.PTS, payload)
			}
		case ipcStop:
			if session != nil {
				session.stop()
				session = nil
			}
			_ = respond(conn, startResponse{OK: true})
		default:
			log.Printf("unknown IPC packet type %d", header.Type)
		}
	}
}

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)
	airplay.SetDebugMode(os.Getenv("OBS_AIRPLAY_DEBUG") != "")
	cache := newDiscoveryCache(context.Background())
	cache.start()
	defer cache.stop()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}
	defer listener.Close()
	fmt.Printf("READY %d\n", listener.Addr().(*net.TCPAddr).Port)
	_ = os.Stdout.Sync()

	conn, err := listener.Accept()
	if err != nil {
		log.Fatal(err)
	}
	handleConnection(conn, cache)
}
