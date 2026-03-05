import axios from 'axios'

// In production (Vercel): VITE_API_URL = "https://your-relay.up.railway.app/sensor-data"
// In local dev: falls back to /api/data (Vite proxy → ESP32 or gateway)
const API_URL = import.meta.env.VITE_API_URL
  ? `${import.meta.env.VITE_API_URL}/sensor-data`
  : '/api/data'

/**
 * Fetch live sensor data.
 * - Production: from cloud relay
 * - Local dev: from ESP32 via Vite proxy
 * NO mock or simulated data — real sensor values only.
 */
export async function fetchSensorData() {
  const res = await axios.get(API_URL, { timeout: 5000 })
  const data = res.data

  if (
    data.temperature === undefined ||
    data.humidity === undefined ||
    data.gasLevel === undefined ||
    data.vibration === undefined
  ) {
    throw new Error('Unexpected response format from sensor source')
  }

  return data
}

