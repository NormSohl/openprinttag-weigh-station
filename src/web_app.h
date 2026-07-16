#pragma once

// Built-in web app (redesign Phase 4+). Replaces Spoolman's web UI with a
// self-hosted inventory dashboard + onboarding form served straight off the
// device. Async (ESPAsyncWebServer) so it never stalls the NFC/scale/display
// tasks. Reads from the local store + config catalog; onboarding writes the
// tag's Main section (via the shared gTagMain/gWriteMainPending path) and
// appends a reconcile event to the log.
//
// Started from syncTask once the network (station or SoftAP) is up.
void webAppBegin();
