package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
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
