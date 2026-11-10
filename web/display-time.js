/**
 * Shared Hub UI datetime display (presentation only).
 * Reads optional window.DBMAP_DISPLAY_TIMEZONE from /display-config.js.
 * Default: Europe/Stockholm, YYYY-MM-DD HH:MM:SS <CET|CEST>.
 */
(function (global) {
  const DEFAULT_TIMEZONE = "Europe/Stockholm";

  function resolveDisplayTimezone() {
    const raw = global.DBMAP_DISPLAY_TIMEZONE;
    if (typeof raw === "string" && raw.trim()) {
      return raw.trim();
    }
    return DEFAULT_TIMEZONE;
  }

  function pad2(value) {
    return String(value).padStart(2, "0");
  }

  /**
   * @param {string|number|Date|null|undefined} value
   * @param {{ empty?: string, invalid?: string }} [options]
   */
  function formatDisplayDateTime(value, options) {
    const emptyLabel =
      options && typeof options.empty === "string" ? options.empty : "Never";
    const invalidLabel =
      options && typeof options.invalid === "string" ? options.invalid : "Unknown";
    if (value === null || value === undefined || value === "") {
      return emptyLabel;
    }

    const date = value instanceof Date ? value : new Date(value);
    if (Number.isNaN(date.getTime())) {
      return invalidLabel;
    }

    let timeZone = resolveDisplayTimezone();
    let parts;
    try {
      parts = new Intl.DateTimeFormat("en-GB", {
        timeZone,
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
        hourCycle: "h23",
        timeZoneName: "short",
      }).formatToParts(date);
    } catch (_err) {
      timeZone = DEFAULT_TIMEZONE;
      parts = new Intl.DateTimeFormat("en-GB", {
        timeZone,
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
        hourCycle: "h23",
        timeZoneName: "short",
      }).formatToParts(date);
    }

    const byType = {};
    for (const part of parts) {
      if (part.type !== "literal") {
        byType[part.type] = part.value;
      }
    }
    const year = byType.year;
    const month = byType.month;
    const day = byType.day;
    const hour = pad2(byType.hour);
    const minute = pad2(byType.minute);
    const second = pad2(byType.second);
    const zone = byType.timeZoneName || timeZone;
    if (!year || !month || !day) {
      return invalidLabel;
    }
    // Force YYYY-MM-DD regardless of en-GB part order.
    return `${year}-${month}-${day} ${hour}:${minute}:${second} ${zone}`;
  }

  global.DBMAP_DISPLAY_TIME_DEFAULTS = {
    timeZone: DEFAULT_TIMEZONE,
    dateOrder: "ymd",
  };
  global.formatDisplayDateTime = formatDisplayDateTime;
  global.resolveDisplayTimezone = resolveDisplayTimezone;
})(typeof window !== "undefined" ? window : globalThis);
