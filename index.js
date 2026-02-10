const fs = require('fs');
const path = require('path');
const { once } = require('events');
const { Client } = require('pg');

const FILE_PATH = path.join(__dirname, 'launch_data.txt');

const client = new Client({
  host: process.env.PGHOST || 'localhost',
  port: Number(process.env.PGPORT || 5432),
  user: process.env.PGUSER || 'postgres',
  password: process.env.PGPASSWORD || 'password',
  database: process.env.PGDATABASE || 'postgres',
  ssl: process.env.PGSSL === 'true' ? { rejectUnauthorized: false } : undefined,
});

async function ensureSchema() {
  await client.query('CREATE EXTENSION IF NOT EXISTS timescaledb');
  await client.query(`
    CREATE TABLE IF NOT EXISTS launch_data (
      time timestamptz PRIMARY KEY,
      temp double precision,
      pressure double precision,
      aqi_pm100_us double precision,
      aqi_pm25_us double precision,
      pm100_env double precision,
      pm25_env double precision,
      pm10_env double precision,
      uv integer
    )
  `);
  await client.query(
    "SELECT create_hypertable('launch_data', 'time', if_not_exists => TRUE, migrate_data => TRUE)"
  );
}

function parseLine(raw) {
  if (!raw || !raw.trim()) return null;

  const [timestampPart, ...restParts] = raw.split(',');
  const tsString = timestampPart?.trim();
  const time = tsString ? new Date(tsString) : null;
  if (!time || Number.isNaN(time.getTime())) {
    console.warn('Skipping line with bad timestamp:', raw);
    return null;
  }

  const tail = restParts.join(',');
  const takeNumber = (regex) => {
    const match = regex.exec(tail);
    return match ? Number(match[1]) : undefined;
  };

  const temp = takeNumber(/T:\s*([-+\d.]+)/i);
  const pressure = takeNumber(/P:\s*([-+\d.]+)/i);
  const uv = takeNumber(/UV:\s*(\d+)/i);
  let aqi_pm100_us = takeNumber(/aqi_pm100_us:\s*([-+\d.]+)/i);
  let aqi_pm25_us = takeNumber(/aqi_pm25_us:\s*([-+\d.]+)/i);
  const pm100_env = takeNumber(/pm100_env:\s*([-+\d.]+)/i);
  const pm25_env = takeNumber(/pm25_env:\s*([-+\d.]+)/i);
  const pm10_env = takeNumber(/pm10_env:\s*([-+\d.]+)/i);

  // Fallback: legacy A:(x, y) maps to aqi_pm100_us/aqi_pm25_us.
  if (aqi_pm100_us === undefined || aqi_pm25_us === undefined) {
    const legacy = /A:\s*\(\s*([-+\d.]+)\s*,\s*([-+\d.]+)\s*\)/i.exec(tail);
    if (legacy) {
      if (aqi_pm100_us === undefined) aqi_pm100_us = Number(legacy[1]);
      if (aqi_pm25_us === undefined) aqi_pm25_us = Number(legacy[2]);
    }
  }

  return {
    time: time.toISOString(),
    temp,
    pressure,
    aqi_pm100_us,
    aqi_pm25_us,
    pm100_env,
    pm25_env,
    pm10_env,
    uv: uv === undefined || Number.isNaN(uv) ? undefined : Math.trunc(uv),
  };
}

async function insertRow(row) {
  const sql = `
    INSERT INTO launch_data (
      time, temp, pressure, aqi_pm100_us, aqi_pm25_us, pm100_env, pm25_env, pm10_env, uv
    ) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)
    ON CONFLICT (time) DO NOTHING
  `;

  const values = [
    row.time,
    row.temp ?? null,
    row.pressure ?? null,
    row.aqi_pm100_us ?? null,
    row.aqi_pm25_us ?? null,
    row.pm100_env ?? null,
    row.pm25_env ?? null,
    row.pm10_env ?? null,
    row.uv ?? null,
  ];

  await client.query(sql, values);
  console.log('Inserted row at time', row.time);
}

async function ensureFile() {
  try {
    await fs.promises.access(FILE_PATH, fs.constants.F_OK);
  } catch {
    await fs.promises.writeFile(FILE_PATH, '', 'utf8');
  }
}

let lastSize = 0;
let lineBuffer = '';
let watchHandle;
let readScheduled = false;
let reading = false;

async function readNewData() {
  if (reading) return;
  reading = true;
  try {
    const stats = await fs.promises.stat(FILE_PATH);
    if (stats.size < lastSize) {
      // File truncated; start over.
      lastSize = 0;
      lineBuffer = '';
    }
    if (stats.size === lastSize) return;

    const stream = fs.createReadStream(FILE_PATH, {
      encoding: 'utf8',
      start: lastSize,
      end: stats.size - 1,
    });

    stream.on('data', (chunk) => {
      lineBuffer += chunk;
      const parts = lineBuffer.split(/\r?\n/);
      lineBuffer = parts.pop() ?? '';
      for (const line of parts) {
        const parsed = parseLine(line);
        if (parsed) {
          console.log('New entry detected:', line.trim());
          insertRow(parsed).catch((err) => {
            console.error('Insert failed:', err.message);
          });
        }
      }
    });

    await once(stream, 'end');
    lastSize = stats.size;
  } finally {
    reading = false;
  }
}

function scheduleRead() {
  if (readScheduled) return;
  readScheduled = true;
  setTimeout(() => {
    readScheduled = false;
    readNewData().catch((err) => console.error('Read error:', err));
  }, 100);
}

async function startWatching() {
  await ensureFile();
  const stats = await fs.promises.stat(FILE_PATH);
  lastSize = stats.size;
  await readNewData();

  watchHandle = fs.watch(FILE_PATH, (eventType) => {
    if (eventType === 'change' || eventType === 'rename') {
      scheduleRead();
    }
  });

  console.log('Watching', FILE_PATH, 'for new launch data lines...');
}

async function main() {
  try {
    await client.connect();
    console.log('Connected to TimescaleDB');
    await ensureSchema();
    console.log('Schema ensured');
    await startWatching();
  } catch (err) {
    console.error('Startup failed:', err);
    process.exit(1);
  }
}

function shutdown() {
  console.log('Shutting down...');
  if (watchHandle) watchHandle.close();
  client.end().catch(() => {});
  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

main();
