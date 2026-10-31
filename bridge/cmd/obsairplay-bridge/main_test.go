package main

import (
	"context"
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
