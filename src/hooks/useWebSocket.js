import { useState, useEffect, useRef, useCallback } from 'react'

/**
 * React hook for WebSocket connection with auto-reconnect.
 *
 * @param {string} url  — WebSocket URL (ws:// or wss://)
 * @param {object} opts — { onMessage, onOpen, onClose, reconnectInterval }
 * @returns {{ connected, send }}
 */
export default function useWebSocket(url, { onMessage, onOpen, onClose, reconnectInterval = 3000 } = {}) {
    const [connected, setConnected] = useState(false)
    const wsRef = useRef(null)
    const reconnectTimer = useRef(null)
    const mountedRef = useRef(true)

    const connect = useCallback(() => {
        if (!url || wsRef.current?.readyState === WebSocket.OPEN) return

        try {
            const ws = new WebSocket(url)

            ws.onopen = () => {
                if (!mountedRef.current) return
                setConnected(true)
                onOpen?.()
            }

            ws.onmessage = (event) => {
                if (!mountedRef.current) return
                try {
                    const data = JSON.parse(event.data)
                    onMessage?.(data)
                } catch {
                    // ignore non-JSON messages
                }
            }

            ws.onclose = () => {
                if (!mountedRef.current) return
                setConnected(false)
                wsRef.current = null
                onClose?.()

                // Auto-reconnect
                reconnectTimer.current = setTimeout(() => {
                    if (mountedRef.current) connect()
                }, reconnectInterval)
            }

            ws.onerror = () => {
                // onclose will fire after this, triggering reconnect
                ws.close()
            }

            wsRef.current = ws
        } catch {
            // WebSocket constructor failed — retry
            reconnectTimer.current = setTimeout(() => {
                if (mountedRef.current) connect()
            }, reconnectInterval)
        }
    }, [url, onMessage, onOpen, onClose, reconnectInterval])

    useEffect(() => {
        mountedRef.current = true
        connect()

        return () => {
            mountedRef.current = false
            clearTimeout(reconnectTimer.current)
            if (wsRef.current) {
                wsRef.current.onclose = null  // prevent reconnect on unmount
                wsRef.current.close()
            }
        }
    }, [connect])

    const send = useCallback((data) => {
        if (wsRef.current?.readyState === WebSocket.OPEN) {
            wsRef.current.send(typeof data === 'string' ? data : JSON.stringify(data))
        }
    }, [])

    return { connected, send }
}
