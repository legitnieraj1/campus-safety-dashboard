import { useState, useEffect, useRef, useCallback } from 'react'
import { fetchSensorData } from '../services/sensorService'
import useWebSocket from './useWebSocket'

const MAX_POINTS = 20
const POLL_INTERVAL = 2000

/**
 * Build the WebSocket URL from the VITE_API_URL env var.
 * https://... → wss://...
 * http://...  → ws://...
 * Empty       → null (WebSocket disabled, use polling only)
 */
function getWsUrl() {
  const apiUrl = import.meta.env.VITE_API_URL
  if (!apiUrl) return null
  return apiUrl.replace(/^http/, 'ws') + '/ws'
}

export default function useSensorData() {
  const [current, setCurrent] = useState(null)
  const [history, setHistory] = useState([])
  const [status, setStatus] = useState('connecting') // connecting | online | offline
  const [lastUpdated, setLastUpdated] = useState(null)
  const [alertActive, setAlertActive] = useState(false)
  const [error, setError] = useState(null)
  const intervalRef = useRef(null)
  const wsConnected = useRef(false)

  // ── Process incoming sensor data (shared by WS and HTTP) ──
  const processData = useCallback((data) => {
    if (
      data.temperature === undefined ||
      data.humidity === undefined ||
      data.gasLevel === undefined ||
      data.vibration === undefined
    ) return  // skip invalid data

    const ts = new Date()

    setCurrent(data)
    setLastUpdated(ts)
    setStatus('online')
    setAlertActive(data.alert !== 'System Normal')
    setError(null)

    setHistory(prev => {
      const entry = {
        ...data,
        time: ts.toLocaleTimeString('en-US', {
          hour12: false,
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
        }),
      }
      const next = [...prev, entry]
      return next.length > MAX_POINTS ? next.slice(next.length - MAX_POINTS) : next
    })
  }, [])

  // ── WebSocket (primary — instant push) ────────────────────
  const wsUrl = getWsUrl()

  const { connected: wsIsConnected } = useWebSocket(wsUrl, {
    onMessage: (data) => {
      wsConnected.current = true
      processData(data)
    },
    onOpen: () => {
      wsConnected.current = true
      // Stop HTTP polling — WebSocket is active
      if (intervalRef.current) {
        clearInterval(intervalRef.current)
        intervalRef.current = null
      }
    },
    onClose: () => {
      wsConnected.current = false
      // WebSocket lost — fall back to HTTP polling
      if (!intervalRef.current) {
        startPolling()
      }
    },
  })

  // ── HTTP polling (fallback) ───────────────────────────────
  const poll = useCallback(async () => {
    // Skip if WebSocket is delivering data
    if (wsConnected.current) return

    try {
      const data = await fetchSensorData()
      processData(data)
    } catch (e) {
      setStatus('offline')
      setError('Cannot reach sensor data source')
    }
  }, [processData])

  const startPolling = useCallback(() => {
    if (intervalRef.current) return
    poll()
    intervalRef.current = setInterval(poll, POLL_INTERVAL)
  }, [poll])

  // ── Start on mount ────────────────────────────────────────
  useEffect(() => {
    // If no WebSocket URL, use polling from the start
    if (!wsUrl) {
      startPolling()
    } else {
      // Give WebSocket 3s to connect, then fall back to polling
      const fallbackTimer = setTimeout(() => {
        if (!wsConnected.current) {
          startPolling()
        }
      }, 3000)
      return () => clearTimeout(fallbackTimer)
    }

    return () => {
      if (intervalRef.current) clearInterval(intervalRef.current)
    }
  }, [wsUrl, startPolling])

  return { current, history, status, lastUpdated, alertActive, error }
}
