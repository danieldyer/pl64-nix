# An absent receiver is restarted by udev when it reappears. A connected but
# stalled/reset receiver needs gpsd's exclusive preStart configuration again.
# Do not interrupt an already-running configuration or start an absent device.
if [[ "${SERVICE_RESULT:-}" == exit-code && "${EXIT_STATUS:-}" == 75 ]] &&
   systemctl is-active --quiet gpsd.service; then
  systemctl --no-block restart gpsd.service
fi
