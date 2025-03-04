from panda import Panda
from opendbc.car import get_safety_config, structs
from opendbc.car.hyundai.hyundaicanfd import CanBus
from opendbc.car.hyundai.values import (HyundaiFlags, CAR, DBC, CAMERA_SCC_CAR, CANFD_RADAR_SCC_CAR,
                                        CANFD_UNSUPPORTED_LONGITUDINAL_CAR, UNSUPPORTED_LONGITUDINAL_CAR, Buttons,
                                        HyundaiExFlags)
from opendbc.car.hyundai.radar_interface import RADAR_START_ADDR
from opendbc.car.interfaces import CarInterfaceBase
from opendbc.car.disable_ecu import disable_ecu

import copy
from openpilot.common.params import Params

ButtonType = structs.CarState.ButtonEvent.Type
Ecu = structs.CarParams.Ecu

# Cancel button can sometimes be ACC pause/resume button, main button can also enable on some cars
ENABLE_BUTTONS = (ButtonType.accelCruise, ButtonType.decelCruise, ButtonType.cancel, ButtonType.mainCruise)

SteerControlType = structs.CarParams.SteerControlType


class CarInterface(CarInterfaceBase):
  @staticmethod
  def _get_params(ret: structs.CarParams, candidate, fingerprint, car_fw, experimental_long, docs) -> structs.CarParams:
    ret.carName = "hyundai"

    cam_can = CanBus(None, fingerprint).CAM
    hda2 = 0x50 in fingerprint[cam_can] or 0x110 in fingerprint[cam_can] or Params().get_bool("IsHda2")
    CAN = CanBus(None, fingerprint, hda2)

    if ret.flags & HyundaiFlags.CANFD:
      # Shared configuration for CAN-FD cars
      ret.isCanfd = True
      Params().put_bool("IsCanfd", True)
      Params().put_bool("SccOnBus2", False)

      ret.enableBsm = 0x1e5 in fingerprint[CAN.ECAN]

      if 0x60 in fingerprint[CAN.ECAN]:
        ret.exFlags |= HyundaiExFlags.AUTOHOLD.value
      if 0x3a0 in fingerprint[CAN.ECAN]:
        ret.exFlags |= HyundaiExFlags.TPMS.value
      if 0x1fa in fingerprint[CAN.ECAN]:
        ret.exFlags |= HyundaiExFlags.NAVI.value
      if {0x1AA, 0x1CF} & set(fingerprint[CAN.ECAN]):
        ret.exFlags |= HyundaiExFlags.LFA.value

      ret.radarUnavailable = RADAR_START_ADDR not in fingerprint[1] or DBC[ret.carFingerprint]["radar"] is None
      ret.experimentalLongitudinalAvailable = candidate not in (CANFD_UNSUPPORTED_LONGITUDINAL_CAR | CANFD_RADAR_SCC_CAR)
      ret.pcmCruise = not ret.openpilotLongitudinalControl

      if 0x105 in fingerprint[CAN.ECAN]:
        ret.flags |= HyundaiFlags.HYBRID.value

      # detect HDA2 with ADAS Driving ECU
      if hda2:
        ret.flags |= HyundaiFlags.CANFD_HDA2.value
        if 0x110 in fingerprint[CAN.CAM]:
          ret.flags |= HyundaiFlags.CANFD_HDA2_ALT_STEERING.value
      else:
        # non-HDA2
        if not ret.flags & HyundaiFlags.RADAR_SCC:
          ret.flags |= HyundaiFlags.CANFD_CAMERA_SCC.value

      if 0x1cf not in fingerprint[CAN.ECAN]:
        ret.flags |= HyundaiFlags.CANFD_ALT_BUTTONS.value

      # Some HDA2 cars have alternative messages for gear checks
      # ICE cars do not have 0x130; GEARS message on 0x40 or 0x70 instead
      if 0x130 not in fingerprint[CAN.ECAN]:
        if 0x40 not in fingerprint[CAN.ECAN]:
          ret.flags |= HyundaiFlags.CANFD_ALT_GEARS_2.value
        else:
          ret.flags |= HyundaiFlags.CANFD_ALT_GEARS.value

      cfgs = [get_safety_config(structs.CarParams.SafetyModel.hyundaiCanfd), ]
      if CAN.ECAN >= 4:
        cfgs.insert(0, get_safety_config(structs.CarParams.SafetyModel.noOutput))
      ret.safetyConfigs = cfgs

      if ret.flags & HyundaiFlags.CANFD_HDA2:
        ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_CANFD_HDA2
        if ret.flags & HyundaiFlags.CANFD_HDA2_ALT_STEERING:
          ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_CANFD_HDA2_ALT_STEERING
      if ret.flags & HyundaiFlags.CANFD_ALT_BUTTONS:
        ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_CANFD_ALT_BUTTONS
      if ret.flags & HyundaiFlags.CANFD_CAMERA_SCC:
        ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_CAMERA_SCC

    else:
      # Shared configuration for non CAN-FD cars
      ret.isCanfd = False
      Params().put_bool("IsCanfd", False)

      ret.enableBsm = 0x58b in fingerprint[0]

      if 0x47f in fingerprint[0]:
        ret.exFlags |= HyundaiExFlags.AUTOHOLD.value
      if 0x593 in fingerprint[0]:
        ret.exFlags |= HyundaiExFlags.TPMS.value
      if 0x544 in fingerprint[0]:
        ret.exFlags |= HyundaiExFlags.NAVI.value
      if 0x391 in fingerprint[0]:
        ret.exFlags |= HyundaiExFlags.LFA.value

      ret.sccBus = 2 if Params().get_bool("SccOnBus2") else 0

      if ret.sccBus == 2:
        Params().put_bool("ExperimentalLongitudinalEnabled", True)
        if any(0x50a in fingerprint[i] for i in [0, 2]):
          ret.exFlags |= HyundaiExFlags.SCC13.value
        if any(0x389 in fingerprint[i] for i in [0, 2]):
          ret.exFlags|=  HyundaiExFlags.SCC14.value
        ret.radarTimeStep = (1.0 / 50)  # 50Hz   SCC11, RadarTrack은 50Hz
      else:
        ret.radarTimeStep = (1.0 / 20)  # 20Hz  RadarTrack 20Hz

      ret.radarUnavailable = ret.sccBus == -1
      ret.experimentalLongitudinalAvailable = not ret.radarUnavailable

      if ret.openpilotLongitudinalControl and ret.sccBus == 0:
        ret.pcmCruise = False
      else:
        ret.pcmCruise = True

      # Send LFA message on cars with HDA
      if 0x485 in fingerprint[2]:
        ret.flags |= HyundaiFlags.SEND_LFA.value

      # These cars use the FCA11 message for the AEB and FCW signals, all others use SCC12
      if 0x38d in fingerprint[0] or 0x38d in fingerprint[2]:
        ret.flags |= HyundaiFlags.USE_FCA.value
      #if 0x483 in fingerprint[0] or 0x483 in fingerprint[2]:
      #  ret.flags |= HyundaiFlags.SEND_FCA12.value

      if ret.flags & HyundaiFlags.LEGACY:
        # these cars require a special panda safety mode due to missing counters and checksums in the messages
        ret.safetyConfigs = [get_safety_config(structs.CarParams.SafetyModel.hyundaiLegacy)]
      else:
        ret.safetyConfigs = [get_safety_config(structs.CarParams.SafetyModel.hyundai, 0)]
      #if candidate in CAMERA_SCC_CAR:
      #  ret.safetyConfigs[0].safetyParam |= Panda.FLAG_HYUNDAI_CAMERA_SCC


    # Common lateral control setup

    ret.centerToFront = ret.wheelbase * 0.4
    ret.steerActuatorDelay = 0.2 # 0.1
    ret.steerLimitTimer = 2.0 # 0.4

    if ret.flags & HyundaiFlags.ANGLE_CONTROL:
      ret.steerControlType = SteerControlType.angle
    else:
      CarInterfaceBase.configure_torque_tune(candidate, ret.lateralTuning)

    # Common longitudinal control setup

    ret.openpilotLongitudinalControl = experimental_long and ret.experimentalLongitudinalAvailable
    ret.startingState = True
    ret.vEgoStarting = 0.3
    ret.startAccel = 2.0
    ret.stoppingDecelRate = 1.0
    ret.vEgoStopping = 0.3
    ret.stopAccel = -3.5

    ret.longitudinalActuatorDelay = 0.5

    if ret.openpilotLongitudinalControl:
      ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_LONG
    if ret.flags & HyundaiFlags.HYBRID:
      ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_HYBRID_GAS
    elif ret.flags & HyundaiFlags.EV:
      ret.safetyConfigs[-1].safetyParam |= Panda.FLAG_HYUNDAI_EV_GAS

    return ret


  @staticmethod
  def init(CP, can_recv, can_send):
    radar_track = Params().get_bool("RadarTrackEnable")
    if all([CP.openpilotLongitudinalControl, radar_track]):
      addr, bus = 0x7d0, 0
      if CP.flags & HyundaiFlags.CANFD_HDA2.value:
        addr, bus = 0x730, CanBus(CP).ECAN
      disable_ecu(can_recv, can_send, bus=bus, addr=addr, com_cont_req=b'\x28\x83\x01')

    # for blinkers
    if CP.flags & HyundaiFlags.ENABLE_BLINKERS:
      disable_ecu(can_recv, can_send, bus=CanBus(CP).ECAN, addr=0x7B1, com_cont_req=b'\x28\x83\x01')

  @staticmethod
  def get_params_adjust_set_speed(CP):
    if CP.flags & HyundaiFlags.CANFD:
      return [16], [20]
    return [16, 20], [12, 14, 16, 18]

  def create_buttons(self, button):
    if self.CP.flags & HyundaiFlags.CANFD:
      if self.CP.flags & HyundaiFlags.CANFD_ALT_BUTTONS:
        return self.create_buttons_canfd_alt(button)
      return self.create_buttons_canfd(button)
    else:
      return self.create_buttons_can(button)

  def create_buttons_can(self, button):
    values = copy.copy(self.CS.clu11)
    values["CF_Clu_CruiseSwState"] = button
    values["CF_Clu_AliveCnt1"] = (values["CF_Clu_AliveCnt1"] + 1) % 0x10
    return self.CC.packer.make_can_msg("CLU11", self.CP.sccBus, values)

  def create_buttons_canfd(self, button):
    values = {
      "COUNTER": self.CS.buttons_counter + 1,
      "SET_ME_1": 1,
      "CRUISE_BUTTONS": button,
    }
    bus = self.CC.CAN.ECAN if self.CP.flags & HyundaiFlags.CANFD_HDA2 else self.CC.CAN.CAM
    return self.CC.packer.make_can_msg("CRUISE_BUTTONS", bus, values)

  def create_buttons_canfd_alt(self, button):
    values = copy.copy(self.CS.canfd_buttons)
    values["CRUISE_BUTTONS"] = button
    values["COUNTER"] = (values["COUNTER"] + 1) % 256
    bus = self.CC.CAN.ECAN if self.CP.flags & HyundaiFlags.CANFD_HDA2 else self.CC.CAN.CAM
    return self.CC.packer.make_can_msg("CRUISE_BUTTONS_ALT", bus, values)

