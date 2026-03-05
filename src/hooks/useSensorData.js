import { useState, useEffect, useRef, useCallback } from 'react'
import { fetchSensorData } from '../services/sensorService'

const MAX_POINTS = 20
const POLL_INTERVAL = 2000

export default function useSensorData() {
  const [current, setCurrent] = useState(null)
  const [history, setHistory] = useState([])
  const [status, setStatus] = useState('connecting') // connecting | online | offline
  const [lastUpdated, setLastUpdated] = useState(null)
  const [alertActive, setAlertActive] = useState(false)
  const [error, setError] = useState(null)
  const intervalRef = useRef(null)

  const poll = useCallback(async () => {
    try {
      const data = await fetchSensorData()
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
    } catch (e) {
      setStatus('offline')
      setError('ESP32 Offline — Cannot reach 192.168.4.1')
      // Do NOT update current data — keep last known values visible but greyed
    }
  }, [])

  useEffect(() => {
    poll()
    intervalRef.current = setInterval(poll, POLL_INTERVAL)
    return () => clearInterval(intervalRef.current)
  }, [poll])

  return { current, history, status, lastUpdated, alertActive, error }
}
