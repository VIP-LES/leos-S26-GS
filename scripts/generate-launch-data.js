#!/usr/bin/env node

// Configurable settings (override by editing these or setting env vars)
const OUTPUT_PATH = process.env.OUTPUT_PATH || "launch_data.txt"; // relative or absolute path to output file
const INTERVAL_MS = Number(process.env.INTERVAL_MS || 1000); // delay between writes
const COUNT = process.env.COUNT ? Number(process.env.COUNT) : null; // number of lines to write; null = infinite
const START_TIME = process.env.START_TIME || null; // ISO string seed; defaults to now if null
const UTC_TIME = false // use UTC format like sample

const fs = require("fs");
const path = require("path");

const weekdays = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

function pad2(n) {
  return n < 10 ? ` ${n}` : String(n);
}

function formatTimestamp(ts) {
  const getDay = UTC_TIME ? ts.getUTCDay() : ts.getDay();
  const getMonth = UTC_TIME ? ts.getUTCMonth() : ts.getMonth();
  const getDate = UTC_TIME ? ts.getUTCDate() : ts.getDate();
  const getHours = UTC_TIME ? ts.getUTCHours() : ts.getHours();
  const getMinutes = UTC_TIME ? ts.getUTCMinutes() : ts.getMinutes();
  const getSeconds = UTC_TIME ? ts.getUTCSeconds() : ts.getSeconds();
  const getYear = UTC_TIME ? ts.getUTCFullYear() : ts.getFullYear();

  const dow = weekdays[getDay];
  const mon = months[getMonth];
  const day = pad2(getDate);
  const hh = String(getHours).padStart(2, "0");
  const mm = String(getMinutes).padStart(2, "0");
  const ss = String(getSeconds).padStart(2, "0");
  const year = getYear;
  return `${dow} ${mon} ${day} ${hh}:${mm}:${ss} ${year}`;
}

function randomFloat(min, max, decimals = 2) {
  const n = Math.random() * (max - min) + min;
  return Number(n.toFixed(decimals));
}

function randomInt(min, max) {
  return Math.floor(Math.random() * (max - min + 1)) + min;
}

function makeLine(ts) {
  const timestamp = formatTimestamp(ts);
  const temperature = randomFloat(20, 40, 2); // Celsius
  const pressure = randomFloat(98000, 103000, 2); // Pascals
  const uvi = randomFloat(0, 11, 1);
  const aqi_pm100_us = randomFloat(0, 25, 1);
  const aqi_pm25_us = randomFloat(0, 25, 1);
  const pm100_env = randomFloat(0, 50, 1);
  const pm25_env = randomFloat(0, 50, 1);
  const pm10_env = randomFloat(0, 50, 1);
  const light_lux = randomFloat(0, 100000, 1);
  const humidity = randomFloat(20, 80, 1);

  return `${timestamp}, temperature:${temperature}, pressure:${pressure}, uvi:${uvi}, aqi_pm100_us:${aqi_pm100_us}, aqi_pm25_us:${aqi_pm25_us}, pm100_env:${pm100_env}, pm25_env:${pm25_env}, pm10_env:${pm10_env}, light_lux:${light_lux}, humidity:${humidity}`;
}

function resolveOutput(p) {
  if (path.isAbsolute(p)) return p;
  return path.join(process.cwd(), p);
}

function sleep(ms) {
  return new Promise((res) => setTimeout(res, ms));
}

function fileEndsWithNewline(target) {
  try {
    const stats = fs.statSync(target);
    if (stats.size === 0) return true;
    const buf = Buffer.alloc(1);
    const fd = fs.openSync(target, "r");
    fs.readSync(fd, buf, 0, 1, stats.size - 1);
    fs.closeSync(fd);
    return buf[0] === 0x0a; // \n
  } catch (err) {
    if (err.code === "ENOENT") return true;
    throw err;
  }
}

async function main() {
  const target = resolveOutput(OUTPUT_PATH);
  fs.mkdirSync(path.dirname(target), { recursive: true });

  let current = START_TIME ? new Date(START_TIME) : new Date();
  if (Number.isNaN(current.getTime())) {
    console.error("Invalid START_TIME; must be ISO string");
    process.exit(1);
  }

  let written = 0;
  let stopAfter = COUNT != null && !Number.isNaN(COUNT) ? COUNT : null;

  const needsNewline = !fileEndsWithNewline(target);
  const stream = fs.createWriteStream(target, { flags: "a" });
  stream.on("error", (err) => {
    console.error("Failed to write:", err);
    process.exit(1);
  });

  console.log(`Writing to ${target}`);
  console.log(`Interval: ${INTERVAL_MS} ms`);
  if (stopAfter) console.log(`Will stop after ${stopAfter} lines`);

  const cleanup = () => {
    stream.end();
    process.exit(0);
  };

  process.on("SIGINT", cleanup);
  process.on("SIGTERM", cleanup);

  if (needsNewline) {
    stream.write("\n");
  }

  while (true) {
    const line = makeLine(current);
    stream.write(line + "\n");
    console.log(`wrote: ${line}`);
    written += 1;
    current = new Date(current.getTime() + INTERVAL_MS);

    if (stopAfter && written >= stopAfter) {
      stream.end();
      break;
    }

    await sleep(INTERVAL_MS);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
