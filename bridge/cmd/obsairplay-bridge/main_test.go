package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func testDevice(name, ip string, port int) airplay.AirPlayDevice {
	return airplay.AirPlayDevice{
		Name: name,
		IP:   ip,
		Port: port,
	}
}

func TestDiscoveryCacheReturnsCachedPreferredImmediately(t *testing.T) {
	cache := newDiscoveryCache(context.Background())
	defer cache.stop()
	cache.observe(testDevice("抖音直播伴侣[721246]", "192.168.3.52", 11292))

	started := time.Now()
	devices, matched := cache.waitFor("抖音直播伴侣", time.Second, time.Second)
	if !matched {
		t.Fatal("expected cached preferred device to match")
	}
	if len(devices) != 1 || devices[0].IP != "192.168.3.52" {
		t.Fatalf("unexpected devices: %+v", devices)
	}
	if elapsed := time.Since(started); elapsed > 50*time.Millisecond {
		t.Fatalf("cached lookup took %v, want an immediate result", elapsed)
	}
}

func TestDiscoveryCacheWaitWakesForPreferredDevice(t *testing.T) {
	cache := newDiscoveryCache(context.Background())
	defer cache.stop()

	result := make(chan bool, 1)
	started := time.Now()
	go func() {
		_, matched := cache.waitFor("抖音直播伴侣", time.Second, time.Second)
		result <- matched
	}()
	time.Sleep(20 * time.Millisecond)
	cache.observe(testDevice("抖音直播伴侣[721246]", "192.168.3.52", 11292))

	select {
	case matched := <-result:
		if !matched {
			t.Fatal("expected preferred device to match")
		}
		if elapsed := time.Since(started); elapsed > 250*time.Millisecond {
			t.Fatalf("preferred wake took %v, want under 250ms", elapsed)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("cache wait did not wake when preferred device arrived")
	}
}

func TestDiscoveryCacheEndpointLookupAndInvalidate(t *testing.T) {
	cache := newDiscoveryCache(context.Background())
	defer cache.stop()
	cache.observe(testDevice("抖音直播伴侣[721246]", "192.168.3.52", 11292))

	device, ok := cache.lookupEndpoint("192.168.3.52", 11292, "抖音直播伴侣")
	if !ok || device.Port != 11292 {
		t.Fatalf("endpoint lookup failed: ok=%t device=%+v", ok, device)
	}

	cache.removeEndpoint("192.168.3.52", 11292)
	if _, ok := cache.lookupEndpoint("192.168.3.52", 11292, "抖音直播伴侣"); ok {
		t.Fatal("invalidated endpoint is still present")
	}
}

func TestStreamSessionReportsDisconnectedState(t *testing.T) {
	_, cancel := context.WithCancel(context.Background())
	session := &streamSession{
		cancel:   cancel,
		finished: make(chan struct{}),
	}
	session.running.Store(true)

	state, message := session.status()
	if state != "running" || message != "" {
		t.Fatalf("unexpected running state: state=%q message=%q", state, message)
	}

	session.fail(errors.New("video data channel closed").Error())
	state, message = session.status()
	if state != "disconnected" {
		t.Fatalf("expected disconnected state, got state=%q message=%q", state, message)
	}
	if message != "video data channel closed" {
		t.Fatalf("unexpected disconnect message: %q", message)
	}

	close(session.finished)
	session.wait(10 * time.Millisecond)
}

func TestStreamSessionStopReportsStoppedState(t *testing.T) {
	_, cancel := context.WithCancel(context.Background())
	session := &streamSession{
		cancel:   cancel,
		finished: make(chan struct{}),
	}
	session.running.Store(true)
	session.stop()

	state, message := session.status()
	if state != "stopped" || message != "" {
		t.Fatalf("unexpected stopped state: state=%q message=%q", state, message)
	}

	close(session.finished)
	session.wait(10 * time.Millisecond)
}

func TestStreamSessionClockMappingIsStableAndSessionLocal(t *testing.T) {
	base := uint64(10 * time.Second)
	first := &streamSession{}
	first.setClock(base)
	first.setClock(base + uint64(5*time.Second))

	if got := first.mapTime(base + uint64(100*time.Millisecond)).Sub(first.mapTime(base)); got != 100*time.Millisecond {
		t.Fatalf("first session clock delta = %v, want 100ms", got)
	}
	if got := first.mapTime(base + uint64(200*time.Millisecond)).Sub(first.mapTime(base)); got != 200*time.Millisecond {
		t.Fatalf("second clock packet moved an established session mapping: delta=%v", got)
	}

	reconnected := &streamSession{}
	reconnectBase := base + uint64(10*time.Minute)
	reconnected.setClock(reconnectBase)
	if got := reconnected.mapTime(reconnectBase + uint64(100*time.Millisecond)).Sub(reconnected.mapTime(reconnectBase)); got != 100*time.Millisecond {
		t.Fatalf("reconnected session clock delta = %v, want 100ms", got)
	}
}

func TestStartRequestRejectsInvalidPlayoutLatency(t *testing.T) {
	for _, latency := range []int{-1, 50, 501} {
		request := startRequest{FPS: 60, Bitrate: 8000, PlayoutLatencyMs: latency}
		if _, err := startSession(context.Background(), request, nil); err == nil ||
			!strings.Contains(err.Error(), "invalid playout latency") {
			t.Fatalf("latency %d error = %v, want playout latency rejection", latency, err)
		}
	}
}

func TestAudioSourcePreservesSubframeAACELDPTS(t *testing.T) {
	chunks := make(chan audioChunk, 2)
	base := time.Unix(1700000000, 0)
	const chunkFrames = 1024
	chunks <- audioChunk{data: make([]byte, chunkFrames*4), pts: base}
	chunks <- audioChunk{
		data: make([]byte, chunkFrames*4),
		pts:  base.Add(pcmSamplesDuration(chunkFrames)),
	}
	source := &audioSource{ctx: context.Background(), ch: chunks}
	frame := make([]byte, 480*4)
	for index := 0; index < 4; index++ {
		pts, err := source.ReadPCMFrame(frame)
		if err != nil {
			t.Fatalf("read frame %d: %v", index, err)
		}
		want := base.Add(pcmSamplesDuration(uint64(index * 480)))
		if !pts.Equal(want) {
			t.Fatalf("frame %d PTS = %v, want %v", index, pts, want)
		}
	}
}

func TestVideoSourceRecoversFromBackpressureAtNextIDR(t *testing.T) {
	source := &videoSource{
		ctx: context.Background(),
		ch:  make(chan videoItem, 3),
	}
	oldGeneration := source.generation.Load()
	source.ch <- videoItem{
		generation: oldGeneration,
		unit:       airplay.VideoAccessUnit{AnnexB: []byte{0, 0, 0, 1, 0x61, 0x01}},
	}
	source.ch <- videoItem{
		generation: oldGeneration,
		unit:       airplay.VideoAccessUnit{AnnexB: []byte{0, 0, 0, 1, 0x41, 0x01}},
	}
	source.beginBackpressureRecovery()
	source.ch <- videoItem{
		generation: source.generation.Load(),
		unit:       airplay.VideoAccessUnit{AnnexB: []byte{0, 0, 0, 1, 0x65, 0x01}},
	}

	unit, err := source.ReadVideoAccessUnit()
	if err != nil {
		t.Fatalf("read recovered access unit: %v", err)
	}
	if !containsIDRNAL(unit.AnnexB) {
		t.Fatalf("recovered access unit is not an IDR: %x", unit.AnnexB)
	}
	if source.awaitingKeyframe.Load() {
		t.Fatal("video source remained in recovery after an IDR")
	}
}

func TestContainsIDRNAL(t *testing.T) {
	for _, test := range []struct {
		name string
		data []byte
		want bool
	}{
		{name: "three-byte start", data: []byte{0, 0, 1, 0x65, 0x01}, want: true},
		{name: "four-byte start", data: []byte{0, 0, 0, 1, 0x65, 0x01}, want: true},
		{name: "non-idr", data: []byte{0, 0, 1, 0x41, 0x01}, want: false},
		{name: "empty", data: nil, want: false},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := containsIDRNAL(test.data); got != test.want {
				t.Fatalf("containsIDRNAL(%x) = %t, want %t", test.data, got, test.want)
			}
		})
	}
}

func TestHandleConnectionReportsIdleStatus(t *testing.T) {
	server, client := net.Pipe()
	defer client.Close()
	_ = client.SetDeadline(time.Now().Add(2 * time.Second))

	done := make(chan struct{})
	go func() {
		handleConnection(server, nil)
		close(done)
	}()

	if err := writePacket(client, ipcHeader{Type: ipcStatus}, nil); err != nil {
		t.Fatalf("write status request: %v", err)
	}
	header, err := readHeader(client)
	if err != nil {
		t.Fatalf("read status response header: %v", err)
	}
	if header.Type != ipcResponse {
		t.Fatalf("unexpected response type: %d", header.Type)
	}
	payload := make([]byte, header.Length)
	if _, err := io.ReadFull(client, payload); err != nil {
		t.Fatalf("read status response: %v", err)
	}
	var response statusResponse
	if err := json.Unmarshal(payload, &response); err != nil {
		t.Fatalf("decode status response: %v", err)
	}
	if !response.OK || response.Session != "idle" {
		t.Fatalf("unexpected status response: %+v", response)
	}

	_ = client.Close()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("bridge handler did not exit after client disconnect")
	}
}
