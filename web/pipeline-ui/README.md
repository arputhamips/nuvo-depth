# Nuvo image pipeline workspace

**Parked prototype.** The conventional console has no link or runtime dependency
on this page. Production installation excludes all `pipeline*` assets and the
gateway does not serve their routes. Source and local frontend build instructions
remain here for future work; building these assets does not enable them on a device.

The standalone device page is `../pipeline.html`. Its own styles and imperative
settings/viewer controller are `../pipeline.css` and `../pipeline.js`. The React
Flow integration lives in `src/canvas.jsx`; editing it does not require editing
the main console's HTML or application script.

Build from this directory with `npm ci` followed by `npm run build`. Commit the
lockfile and the generated `../pipeline-canvas.js` and `../pipeline-canvas.css`.
These self-hosted assets run in the browser. The Pi needs no Node installation,
bundler, React server, or external CDN. Production dependencies and notices are
listed in `../pipeline-licenses.txt`.
The CMake installation excludes both this authoring directory and every pipeline
asset in `web/`. Re-enabling the experiment requires an explicit integration change.

`../device-settings.js` owns the shared settings field definitions, numeric JSON
types, bounds, configuration ETags, CSRF requests, and session draft storage.
`../frame-codec.js` provides bounded frame parsing and image/depth rendering.
Both pages consume these modules. The standalone adapter is `../pipeline-host.js`.
Only the main console loads `../app.js`.

The React graph owns visual positions, selection, pan, and zoom. Device topology
is immutable and `../pipeline-model.js` validates supported output routing and
resource limits. Node coordinates and zoom are stored separately from device
configuration. Dragging never sends configuration requests. All nodes start
collapsed even when their positions are restored. Optional output cards provide
the only removal controls; core nodes and fixed connections cannot be deleted.

The heading's `.pipeline-node-grip` moves a node; its separate arrow opens settings.
Wire ports are separate,
named controls with keyboard activation. The fixed interaction panel provides
44px targets and non-dragging movement/connection alternatives at every zoom.
Editable edge hit paths are 44 canvas units wide; stroke styling must target
`.react-flow__edge-path`, never all paths, or it will shrink the invisible hit path.
Only optional output edges reconnect at either end. Encoder edges reconnect only
at the source. Invalid drops never delete a route. Reconnection and disconnection
go through the immutable routing model, so Undo and admission guards still apply.
The locally persisted viewport also stores canvas dimensions and recentres after
large viewport changes. Reduced motion preferences disable viewport transitions.

For browser QA, use a local synthetic device fixture. Do not claim hardware
validation from fixture results. The gateway integration suite verifies that
pipeline routes return 404, including when stale assets exist in the web root.

## Parked prototype reference

**Image pipeline** is another view of the existing camera settings and output
subscriptions. Exposure shares Capture's draft, validation, Apply, and Discard;
Matcher and Disparity filters share the Matcher draft; Monitoring encoder shares
Streaming's draft. Applying either matcher node saves the entire shared Matcher
draft, including filter edits. Calibration and Depth profiles open their existing
workflows. Changes on either view appear in the other, with the same configuration
revision checks and device confirmation.

The processing path is fixed: Exposure controls the stereo cameras; images pass
through rectification, stereo matching, disparity filtering, and depth conversion
as required by requested products. Core nodes cannot be removed or rewired.
Matcher output is internal; the public disparity output follows filtering.

1. Tap a stage's output socket on its right edge, choose a source product, then tap
   the destination's input socket on its left edge or choose a destination and
   **Connect**. Dragging between sockets is also supported. Small socket dots have
   larger invisible hit areas; the pointer becomes a crosshair over wire targets.
   **Connect ports** and the source/destination menus provide equivalent keyboard
   and single-tap controls. No dragging is required.
2. Use **Add output node** for an additional branch. **Rectified + depth** creates
   parallel rectified-stereo and metric-depth outputs. It replaces the draft's raw
   outputs; Undo restores the previous draft.
3. Set the shared raw cadence and delivery policy, then **Apply outputs**.
   Unconnected enabled outputs, unsupported products, oversized frames, excessive
   bandwidth, and unqualified Exact delivery are rejected. Server admission still
   enforces resource and transport limits independently.
4. Click an output node or choose **Preview output** to see its image in the small
   viewer on the same page. Preview follows applied routes; unapplied changes show
   Pending. Depth/disparity are colour-mapped, and provisional metric calibration
   remains labelled. A monitoring output previews the configured compressed stream;
   enable its encoder in the shared Streaming settings first.

Output routes, enable switches, and cadence apply to this browser, using the same
selection as **Outputs** and **Live view**. Removing an optional output stops its
request after Apply; other clients keep their own subscriptions. Duplicate output
nodes share one product request. Image-only branches do not request stereo matching.
An empty raw output set stops raw demand while allowing a monitoring output.

The workspace is a separate page, **pipeline.html**, linked from the main console.
The image path starts from left to right. All nodes start collapsed; use a node's
heading's arrow button to expand it, or **Collapse all** to close them. Pending settings
survive navigation to the original settings page and back, retaining their edit-base
revision. Apply and Discard stay available on nodes with pending changes.

Drag anywhere on the node's heading bar except its arrow button to move it. The
heading shows a grab cursor; the arrow opens settings independently. **Move node**
also provides direction buttons and exact coordinates. Drag the background to pan, and scroll or use the
zoom buttons to zoom. **Fit** frames the graph; **Arrange** restores the initial
positions. After selecting a node with Enter, arrow keys also move it. Positions
and zoom are saved locally and never change the device's processing order. Valid
public source handles can connect to optional output inputs; required processing
connections remain protected. Source/destination menus provide an alternative.

Select an editable wire, or choose it in **Wire** and **Edit wire**, to change its
source or move it to an empty output. Mouse users can also drag its wire-end
handles; touch users can drag an IN port to a different OUT port to replace its source.
**Disconnect wire** clears the route, switches that output off, and preserves its
node. **Undo** restores the previous route. Occupied destinations cannot be silently
overwritten when moving a wire. Invalid drops and Escape preserve the old route.
The encoder's input is required while enabled; its source can change, and disabling
its encoder uses the shared Streaming settings. Fixed camera/processing links have
no editing controls. Route changes still require **Apply outputs**.

Node actions, ports at readable zoom, and the unscaled interaction controls use
44 CSS-pixel targets. **Fit** provides a zoomed-out overview; **Focus node** returns
to readable zoom. The unscaled controls provide equivalent actions at any zoom.
On small screens, **Node and wire controls** folds away, editing tools use one
contextual panel, and settings open in a full-size sheet with a fixed Apply/Discard
footer. Closing the sheet returns to the collapsed node. The viewer minimizes
while editing so it cannot cover controls.
These interactions follow the WCAG 2.2 guidance for
[target size](https://www.w3.org/WAI/WCAG22/Understanding/target-size-enhanced.html)
and [alternatives to dragging](https://www.w3.org/WAI/WCAG22/Understanding/dragging-movements).

The small viewer is docked in the lower-right corner of the workspace, outside the
zoomed canvas. Clicking an output opens it. Its minus/plus control minimizes or
expands it; small screens start with it minimized. Minimizing pauses encoded
preview demand; explicitly applied raw output subscriptions remain active. **Output delivery** folds away
the shared cadence and delivery options.

React Flow, its styles, and its dependencies are bundled locally. The layout,
connection lines, draft history, and single preview run in the client browser,
with no server rendering or Node runtime on the Pi. Selection of an already
subscribed raw output adds no stream or Pi processing. Preview drawing is capped
at 5 updates/s; the device still performs requested capture, processing, encoding,
and delivery. The full performance monitor remains on Overview and Diagnostics.
For frontend source and build instructions, see [pipeline workspace](../web/pipeline-ui/README.md).

