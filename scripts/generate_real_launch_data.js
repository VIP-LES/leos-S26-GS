#!/usr/bin/env node

/**
 * generate_real_launch_data.js
 * 
 * Reads sensorlog.txt containing real launch data and transforms it:
 * - Updates timestamps to start from current date/time (or offset)
 * - Converts A:(pm25_aqi, pm100_aqi) to individual aqi/pm fields
 * - Ignores EFM data
 * 
 * Usage:
 *   node scripts/generate_real_launch_data.js
 *   OFFSET_MINS=-15 node scripts/generate_real_launch_data.js   # Start 15 mins ago
 *   OFFSET_MINS=-60 node scripts/generate_real_launch_data.js   # Start 1 hour ago
 * 
 * Environment variables:
 *   INPUT_PATH   - path to input sensor log (default: sensorlog.txt)
 *   OUTPUT_PATH  - path to output file (default: launch_data.txt)
 *   START_TIME   - ISO string for start time (default: now, overrides OFFSET_MINS)
 *   OFFSET_MINS  - minutes offset from now (negative = past, positive = future)
 *                  Examples: -15 (15 mins ago), -30 (30 mins ago), -60 (1 hour ago)
 */

const fs = require("fs");
const path = require("path");

const INPUT_PATH = process.env.INPUT_PATH || "sensorlog.txt";
const OUTPUT_PATH = process.env.OUTPUT_PATH || "launch_data.txt";
const START_TIME = process.env.START_TIME || null;
const OFFSET_MINS = -180

const weekdays = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

function pad2(n) {
  return n < 10 ? ` ${n}` : String(n);
}

function formatTimestamp(ts) {
  const dow = weekdays[ts.getDay()];
  const mon = months[ts.getMonth()];
  const day = pad2(ts.getDate());
  const hh = String(ts.getHours()).padStart(2, "0");
  const mm = String(ts.getMinutes()).padStart(2, "0");
  const ss = String(ts.getSeconds()).padStart(2, "0");
  const year = ts.getFullYear();
  return `${dow} ${mon} ${day} ${hh}:${mm}:${ss} ${year}`;
}

function parseOriginalTimestamp(timestampStr) {
  // Parse format: "Tue Sep  9 09:40:30 2025"
  const parts = timestampStr.trim().split(/\s+/);
  // parts: ["Tue", "Sep", "9", "09:40:30", "2025"]
  const monthStr = parts[1];
  const day = parseInt(parts[2], 10);
  const timeParts = parts[3].split(":");
  const year = parseInt(parts[4], 10);
  
  const monthIndex = months.indexOf(monthStr);
  const hours = parseInt(timeParts[0], 10);
  const minutes = parseInt(timeParts[1], 10);
  const seconds = parseInt(timeParts[2], 10);
  
  return new Date(year, monthIndex, day, hours, minutes, seconds);
}

function parseLine(line) {
  // Format: "Tue Sep  9 09:40:30 2025, T:24.564599609375023, P:99401.9140625, A:(17, 61), UV:2, EFM:(...)"
  
  // Extract timestamp (everything before first comma)
  const firstComma = line.indexOf(",");
  if (firstComma === -1) return null;
  
  const timestampStr = line.substring(0, firstComma);
  
  // Parse the rest of the fields
  const rest = line.substring(firstComma + 1);
  
  // Extract T (temperature)
  const tMatch = rest.match(/T:([\d.]+)/);
  if (!tMatch) return null;
  const temperature = parseFloat(tMatch[1]);
  
  // Extract P (pressure)
  const pMatch = rest.match(/P:([\d.]+)/);
  if (!pMatch) return null;
  const pressure = parseFloat(pMatch[1]);
  
  // Extract A (AQI values) - format: A:(pm25_aqi, pm100_aqi)
  const aMatch = rest.match(/A:\((\d+),\s*(\d+)\)/);
  if (!aMatch) return null;
  const pm25_aqi = parseInt(aMatch[1], 10);
  const pm100_aqi = parseInt(aMatch[2], 10);
  
  // Extract UV
  const uvMatch = rest.match(/UV:(\d+)/);
  if (!uvMatch) return null;
  const uv = parseInt(uvMatch[1], 10);
  
  return {
    timestampStr,
    temperature,
    pressure,
    pm25_aqi,
    pm100_aqi,
    uv
  };
}

function generatePmEnvValues(pm25_aqi, pm100_aqi) {
  // Generate realistic pm_env values based on AQI readings
  // These are approximations since we don't have the actual env values
  
  // PM2.5 env is roughly correlated with PM2.5 AQI
  // AQI 0-50 corresponds roughly to 0-12 µg/m³
  const pm25_env = (pm25_aqi / 50) * 12 + Math.random() * 5;
  
  // PM10 env - estimate based on PM2.5 (typically PM10 > PM2.5)
  const pm10_env = pm25_env * (0.3 + Math.random() * 0.4);
  
  // PM100 (PM1.0) env - typically less than PM2.5
  const pm100_env = pm25_env * (0.6 + Math.random() * 0.3);
  
  return {
    pm25_env: pm25_env.toFixed(1),
    pm10_env: pm10_env.toFixed(1),
    pm100_env: pm100_env.toFixed(1)
  };
}

function transformLine(parsed, newTimestamp) {
  const timestamp = formatTimestamp(newTimestamp);
  const temp = parsed.temperature.toFixed(2);
  const pressure = parsed.pressure.toFixed(2);
  const uvi = parsed.uv.toFixed(1);
  
  // AQI values from the original A field
  const aqi_pm25_us = parsed.pm25_aqi;
  const aqi_pm100_us = parsed.pm100_aqi;
  
  // Generate pm_env values
  const pmEnv = generatePmEnvValues(parsed.pm25_aqi, parsed.pm100_aqi);

  // Generate synthetic humidity and light_lux values
  const humidity = (Math.random() * 60 + 20).toFixed(1);        // 20–80 %
  const light_lux = (Math.random() * 100000).toFixed(1);         // 0–100000 lux
  
  return `${timestamp}, temperature:${temp}, pressure:${pressure}, uvi:${uvi}, aqi_pm100_us:${aqi_pm100_us}, aqi_pm25_us:${aqi_pm25_us}, pm100_env:${pmEnv.pm100_env}, pm25_env:${pmEnv.pm25_env}, pm10_env:${pmEnv.pm10_env}, light_lux:${light_lux}, humidity:${humidity}`;
}

function resolveFilePath(p) {
  if (path.isAbsolute(p)) return p;
  return path.join(process.cwd(), p);
}

function main() {
  const inputPath = resolveFilePath(INPUT_PATH);
  const outputPath = resolveFilePath(OUTPUT_PATH);
  
  console.log(`Reading from: ${inputPath}`);
  console.log(`Writing to: ${outputPath}`);
  
  // Read input file
  if (!fs.existsSync(inputPath)) {
    console.error(`Input file not found: ${inputPath}`);
    process.exit(1);
  }
  
  const inputContent = fs.readFileSync(inputPath, "utf-8");
  const lines = inputContent.split("\n").filter(line => line.trim().length > 0);
  
  console.log(`Found ${lines.length} lines in input file`);
  
  // Parse all lines first to get timestamp information
  const parsedLines = [];
  let firstTimestamp = null;
  
  for (const line of lines) {
    const parsed = parseLine(line);
    if (parsed) {
      const originalTime = parseOriginalTimestamp(parsed.timestampStr);
      if (!firstTimestamp) {
        firstTimestamp = originalTime;
      }
      parsedLines.push({
        ...parsed,
        originalTime,
        offsetMs: originalTime.getTime() - firstTimestamp.getTime()
      });
    } else {
      console.warn(`Skipping unparseable line: ${line.substring(0, 50)}...`);
    }
  }
  
  console.log(`Successfully parsed ${parsedLines.length} lines`);
  
  if (parsedLines.length === 0) {
    console.error("No valid lines found in input file");
    process.exit(1);
  }
  
  // Calculate new timestamps starting from now (or START_TIME, or offset from now)
  let startTime;
  if (START_TIME) {
    startTime = new Date(START_TIME);
  } else {
    startTime = new Date(Date.now() + OFFSET_MINS * 60 * 1000);
  }
  
  if (Number.isNaN(startTime.getTime())) {
    console.error("Invalid START_TIME; must be ISO string");
    process.exit(1);
  }
  
  console.log(`Start time: ${formatTimestamp(startTime)}`);
  if (OFFSET_MINS !== 0 && !START_TIME) {
    console.log(`(Offset: ${OFFSET_MINS} minutes from now)`);
  }
  
  // Transform all lines
  const outputLines = parsedLines.map(parsed => {
    const newTimestamp = new Date(startTime.getTime() + parsed.offsetMs);
    return transformLine(parsed, newTimestamp);
  });
  
  // Write output file
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, outputLines.join("\n") + "\n");
  
  console.log(`\nWrote ${outputLines.length} lines to ${outputPath}`);
  
  // Show time range
  const lastParsed = parsedLines[parsedLines.length - 1];
  const endTime = new Date(startTime.getTime() + lastParsed.offsetMs);
  const durationMs = lastParsed.offsetMs;
  const durationMin = Math.floor(durationMs / 60000);
  const durationSec = Math.floor((durationMs % 60000) / 1000);
  
  console.log(`Time range: ${formatTimestamp(startTime)} to ${formatTimestamp(endTime)}`);
  console.log(`Duration: ${durationMin} minutes ${durationSec} seconds`);
}

main();
