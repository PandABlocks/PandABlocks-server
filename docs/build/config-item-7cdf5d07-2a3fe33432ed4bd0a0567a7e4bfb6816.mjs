/**
 * version-switcher — a MyST plugin + AnyWidget runtime, in a single file.
 *
 * This one `.mjs` is loaded by MyST in two ways:
 *
 *  - **Build time** — MyST imports the module and reads `default.directives`,
 *    registering a `{version-switcher}` directive that emits an `anywidget` AST
 *    node. The node's `esm` points back at *this
 *    file* (see PLUGIN_PATH / relativePath), so there is no second asset and no
 *    `DEFAULT_ESM` URL to host.
 *  - **Run time** — AnyWidget imports the localized copy of this file in the
 *    browser and calls `default.render`, which fetches `switcher.json`, works out
 *    the current version from the page URL, and renders a <select> that navigates
 *    to the same page in the chosen version (falling back to that version's root
 *    when the page doesn't exist there).
 *
 * The module is isomorphic: no Node-only imports, so the same file runs under
 * MyST (Node) and in the browser. The pure URL helpers are exported by name for
 * unit testing without a DOM. Pattern mirrors jupyter-book/myst-plugins'
 * `searchfilter` so this can be upstreamed there with minimal change.
 */

/* --------------------------- self-referential esm ----------------------------
 * The directive sets the anywidget `esm` to a relative path from the document
 * being built to *this* module, so MyST localizes this very file as the runtime.
 */
const PLUGIN_PATH = new URL(import.meta.url).pathname;

/** POSIX relative path from `fromDir` to `toFile` (both absolute, no Node deps). */
export function relativePath(fromDir, toFile) {
	const from = String(fromDir).split("/").filter(Boolean);
	const to = String(toFile).split("/").filter(Boolean);
	let i = 0;
	while (i < from.length && i < to.length && from[i] === to[i]) i += 1;
	const up = from.slice(i).map(() => "..");
	return [...up, ...to.slice(i)].join("/") || ".";
}

/* ----------------------------- pure helpers ----------------------------- */

/** Ensure a pathname ends with exactly one trailing slash. */
export function withTrailingSlash(pathname) {
	if (!pathname) return "/";
	return pathname.endsWith("/") ? pathname : `${pathname}/`;
}

/** Display label for an entry; the pydata `preferred` (stable) entry gets a star. */
export function entryLabel(entry) {
	const base = entry.name || entry.version || entry.url;
	return entry.preferred ? `${base} ★` : base;
}

/**
 * Decide which switcher entry corresponds to the page currently being viewed.
 *
 * Strategy:
 *   1. If `versionMatch` is provided, match it against entry.version (exact),
 *      then loosely (entry.version included in versionMatch, e.g. "2.1.3"
 *      matching the "2.1" entry).
 *   2. Otherwise infer from the URL: the entry whose url *pathname* is the
 *      longest prefix of the current pathname. This is origin-agnostic, so it
 *      works on gh-pages (/REPO/2.1/...) regardless of host.
 *
 * @returns {object|null} the active entry, or null if none matched.
 */
export function detectCurrent(entries, locationPathname, versionMatch) {
	if (!Array.isArray(entries) || entries.length === 0) return null;

	if (versionMatch) {
		const exact = entries.find((e) => e.version === versionMatch);
		if (exact) return exact;
		const loose = entries.find(
			(e) => e.version && String(versionMatch).startsWith(e.version),
		);
		if (loose) return loose;
	}

	let best = null;
	let bestLen = -1;
	for (const e of entries) {
		let base;
		try {
			base = withTrailingSlash(new URL(e.url, "http://x").pathname);
		} catch {
			continue;
		}
		const hay = withTrailingSlash(locationPathname);
		if (hay.startsWith(base) && base.length > bestLen) {
			best = e;
			bestLen = base.length;
		}
	}
	return best;
}

/** Is this a local dev host (localhost / 127.0.0.1 / ::1 / *.localhost)? */
export function isLocalHost(hostname) {
	return (
		hostname === "localhost" ||
		hostname === "127.0.0.1" ||
		hostname === "[::1]" ||
		hostname === "::1" ||
		(typeof hostname === "string" && hostname.endsWith(".localhost"))
	);
}

/**
 * Dev convenience: on a local host, when no switcher version matches the current
 * URL, synthesize a "local" entry rooted at `/` (the origin root) and mark it
 * current. This lets path preservation + the existence probe be exercised
 * locally against the real (gh-pages) version URLs, which otherwise can't happen
 * because `/commands` on localhost sits under no gh-pages version base.
 *
 * Never triggers in production (non-local hostname) or when a real version
 * already matched.
 *
 * @param {object} location  `{ hostname, origin }` (window.location-like)
 * @returns {{entries: Array, current: object|null}}
 */
export function withLocalFallback(entries, current, location) {
	if (current || !isLocalHost(location.hostname)) return { entries, current };
	const local = {
		version: "local",
		name: "local (dev)",
		url: new URL("/", location.origin).href,
	};
	return { entries: [local, ...entries], current: local };
}

/**
 * Compute the URL to navigate to when the user picks `targetEntry`.
 *
 * When `preservePath` is true and we know the current entry, the page path
 * relative to the current version root is carried over to the target version.
 * Otherwise we go to the target version root.
 *
 * @param {object} targetEntry  entry chosen in the dropdown
 * @param {object|null} currentEntry  entry for the page being viewed
 * @param {{pathname:string, hash:string}} location  current location-ish object
 * @param {boolean} preservePath
 * @returns {string} absolute href to navigate to
 */
export function computeTargetUrl(
	targetEntry,
	currentEntry,
	location,
	preservePath,
) {
	const target = new URL(targetEntry.url);
	target.pathname = withTrailingSlash(target.pathname);

	if (preservePath && currentEntry) {
		const currentBase = withTrailingSlash(new URL(currentEntry.url).pathname);
		const here = location.pathname || "";
		const rel = here.startsWith(currentBase)
			? here.slice(currentBase.length)
			: "";
		target.pathname = withTrailingSlash(target.pathname) + rel;
		if (location.hash) target.hash = location.hash;
	}
	return target.href;
}

/**
 * Resolve where to actually navigate, falling back to the target version root
 * when the path-preserved page doesn't exist there.
 *
 * On `/v1/x/y` switching to v2 this returns `/v2/x/y` if that page exists, else
 * `/v2`. The existence check (`pageExists`) is injected so this stays unit
 * -testable in Node; the browser passes the real network probe below.
 *
 * `pageExists(url)` returns `true` (exists), `false` (definitely 404), or `null`
 * (indeterminate — e.g. cross-origin/CORS). Only a definite `false` triggers the
 * root fallback; `null` keeps the path-preserved candidate so a blocked probe
 * never strands users at the root.
 *
 * @returns {Promise<string>} absolute href to navigate to
 */
export async function resolveTargetUrl({
	targetEntry,
	currentEntry,
	location,
	preservePath,
	pageExists,
}) {
	const candidate = computeTargetUrl(
		targetEntry,
		currentEntry,
		location,
		preservePath,
	);

	// Nothing to probe: we're already heading to the version root.
	if (!preservePath || !currentEntry) return candidate;
	const root = computeTargetUrl(targetEntry, currentEntry, location, false);
	if (candidate === root) return candidate;

	const found = await pageExists(candidate);
	return found === false ? root : candidate;
}

/**
 * Browser existence probe for a deep page in the target version.
 * HEAD first (cheap); fall back to GET if the host rejects HEAD. Network/CORS
 * errors are indeterminate (`null`), as is any non-404 error status.
 *
 * @returns {Promise<boolean|null>}
 */
export async function pageExists(url) {
	// `cache: 'no-store'` forces a full response every time. Without it a cache hit
	// can come back as a 304 revalidation, and gh-pages/Fastly strips
	// `Access-Control-Allow-Origin` from 304s — which makes a *cross-origin* probe
	// (e.g. a preview origin → gh-pages) get blocked by the browser. A fresh 200
	// always carries the CORS header. (Same-origin, the production case, is fine
	// either way.)
	const init = {
		method: "HEAD",
		credentials: "omit",
		redirect: "follow",
		cache: "no-store",
	};
	try {
		let res = await fetch(url, init);
		if (res.status === 405 || res.status === 501) {
			res = await fetch(url, { ...init, method: "GET" });
		}
		if (res.ok) return true;
		if (res.status === 404) return false;
		return null;
	} catch {
		return null; // network / CORS — can't tell
	}
}

/* ------------------------------- rendering ------------------------------ */

function buildSelect(entries, currentEntry, onPick) {
	const wrap = document.createElement("div");
	wrap.className = "myst-version-switcher";
	Object.assign(wrap.style, {
		display: "inline-flex",
		alignItems: "center",
		fontFamily: "inherit",
		fontSize: "0.875rem",
	});

	const select = document.createElement("select");
	// Match the sibling navbar controls (search pill / theme toggle): a soft
	// translucent fill + border derived from the inherited text colour, so it
	// reads correctly in both light and dark without hard-coding theme colours.
	select.setAttribute("aria-label", "Select documentation version");
	Object.assign(select.style, {
		font: "inherit",
		color: "inherit",
		background: "color-mix(in srgb, currentColor 6%, transparent)",
		border: "1px solid color-mix(in srgb, currentColor 22%, transparent)",
		borderRadius: "0.5rem",
		padding: "0.35em 1.6em 0.35em 0.6em",
		cursor: "pointer",
		maxWidth: "16em",
	});

	if (!currentEntry) {
		const placeholder = document.createElement("option");
		placeholder.textContent = "Choose version…";
		placeholder.value = "";
		placeholder.disabled = true;
		placeholder.selected = true;
		select.appendChild(placeholder);
	}

	entries.forEach((entry, i) => {
		const opt = document.createElement("option");
		opt.value = String(i);
		opt.textContent = entryLabel(entry);
		if (currentEntry && entry === currentEntry) opt.selected = true;
		select.appendChild(opt);
	});

	select.addEventListener("change", () => {
		const idx = Number(select.value);
		if (Number.isInteger(idx) && entries[idx]) onPick(entries[idx], select);
	});

	wrap.appendChild(select);
	return wrap;
}

function showError(el, message) {
	const err = document.createElement("span");
	err.textContent = message;
	Object.assign(err.style, { fontSize: "0.8rem", opacity: "0.7" });
	el.appendChild(err);
}

/** AnyWidget runtime entry point (`default.render`). */
export async function render({ model, el }) {
	const jsonUrl = model.get("json_url");
	const versionMatch = model.get("version_match"); // optional override
	const preservePath = model.get("preserve_path") !== false; // default true
	const probeTarget = model.get("probe_target") !== false; // default true

	if (!jsonUrl) {
		showError(el, "version-switcher: no json_url configured.");
		return () => {
			el.innerHTML = "";
		};
	}

	try {
		const resolved = new URL(jsonUrl, window.location.href).href;
		const res = await fetch(resolved, { credentials: "omit" });
		if (!res.ok) throw new Error(`HTTP ${res.status}`);
		const raw = await res.json();

		const detected = detectCurrent(raw, window.location.pathname, versionMatch);
		// On localhost, fall back to a synthetic "local" version rooted at `/` so
		// the switcher is usable in `myst start` against live version URLs.
		const { entries, current } = withLocalFallback(
			raw,
			detected,
			window.location,
		);

		const ui = buildSelect(entries, current, async (targetEntry, select) => {
			if (current && targetEntry === current) return;
			// Disable while probing so a slow HEAD can't be double-triggered.
			if (select) select.disabled = true;
			try {
				const href = await resolveTargetUrl({
					targetEntry,
					currentEntry: current,
					location: {
						pathname: window.location.pathname,
						hash: window.location.hash,
					},
					preservePath,
					// When probing is off, claim "exists" so we keep the path unchanged.
					pageExists: probeTarget ? pageExists : async () => true,
				});
				window.location.assign(href);
			} finally {
				if (select) select.disabled = false;
			}
		});

		el.appendChild(ui);
	} catch (err) {
		showError(el, "Could not load version list.");
		// eslint-disable-next-line no-console
		console.error("[version-switcher]", err);
	}

	return () => {
		el.innerHTML = "";
	};
}

/* ----------------------- build-time MyST directive ---------------------- */

let counter = 0;
function uid() {
	counter += 1;
	return `version-switcher-${counter}`;
}

const versionSwitcherDirective = {
	name: "version-switcher",
	doc: "A pydata-style documentation version switcher, rendered via anywidget.",
	options: {
		"json-url": {
			type: String,
			required: true,
			doc: "URL (absolute or root-relative) to a pydata-format switcher.json.",
		},
		"version-match": {
			type: String,
			required: false,
			doc: 'Force the "current" version instead of auto-detecting from the URL.',
		},
		"preserve-path": {
			type: Boolean,
			required: false,
			doc: "Carry the current page path across versions (default: true).",
		},
		"probe-target": {
			type: Boolean,
			required: false,
			doc:
				"Probe the target page and fall back to the version root if it 404s " +
				"(default: true). Set false for cross-origin switchers where the probe " +
				"is CORS-blocked.",
		},
		class: {
			type: String,
			required: false,
			doc: "Extra class names for the widget container.",
		},
	},
	run(data, vfile) {
		const opts = data.options ?? {};
		// Point the anywidget at this very file, relative to the document being built,
		// unless a dev override is supplied.
		const fromDir = String(vfile?.path || "").replace(/\/[^/]*$/, "");
		const esm = relativePath(fromDir, PLUGIN_PATH);
		const model = {
			json_url: opts["json-url"],
			version_match: opts["version-match"],
			// default true unless explicitly set to false
			preserve_path: opts["preserve-path"] !== false,
			probe_target: opts["probe-target"] !== false,
		};
		return [
			{
				type: "anywidget",
				esm,
				id: uid(),
				model,
				class: opts.class,
			},
		];
	},
};

const plugin = {
	name: "version-switcher",
	directives: [versionSwitcherDirective],
	// `render` lives on the default export so the same file works as the anywidget
	// runtime module (AnyWidget reads `default.render`).
	render,
};

export default plugin;
