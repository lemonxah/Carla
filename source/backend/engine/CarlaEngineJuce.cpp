/*
 * Carla Plugin Host
 * Copyright (C) 2011-2020 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the GPL.txt file
 */

#include "CarlaEngineGraph.hpp"
#include "CarlaEngineInit.hpp"
#include "CarlaEngineInternal.hpp"
#include "CarlaBackendUtils.hpp"
#include "CarlaStringList.hpp"

#include "RtLinkedList.hpp"

#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdouble-promotion"
# pragma GCC diagnostic ignored "-Weffc++"
# pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

#include "AppConfig.h"
#include "juce_audio_devices/juce_audio_devices.h"

#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
# pragma GCC diagnostic pop
#endif

CARLA_BACKEND_START_NAMESPACE

// -------------------------------------------------------------------------------------------------------------------

struct MidiInPort {
    juce::MidiInput* port;
    char name[STR_MAX];
    char identifier[STR_MAX];
};

struct MidiOutPort {
    juce::MidiOutput* port;
    char name[STR_MAX];
    char identifier[STR_MAX];
};

struct RtMidiEvent {
    uint64_t time; // needs to compare to internal time
    uint8_t  size;
    uint8_t  data[EngineMidiEvent::kDataSize];
};

// -------------------------------------------------------------------------------------------------------------------
// Fallback data

static const MidiInPort  kMidiInPortFallback    = { nullptr, { '\0' }, { '\0' } };
static /* */ MidiInPort  kMidiInPortFallbackNC  = { nullptr, { '\0' }, { '\0' } };
static const MidiOutPort kMidiOutPortFallback   = { nullptr, { '\0' }, { '\0' } };
static /* */ MidiOutPort kMidiOutPortFallbackNC = { nullptr, { '\0' }, { '\0' } };
static const RtMidiEvent kRtMidiEventFallback   = { 0, 0, { 0 } };

// -------------------------------------------------------------------------------------------------------------------
// Global static data

static CharStringListPtr gDeviceNames;
static juce::OwnedArray<juce::AudioIODeviceType> gDeviceTypes;

struct JuceCleanup : public juce::DeletedAtShutdown {
    JuceCleanup() noexcept {}
    ~JuceCleanup()
    {
        gDeviceTypes.clear(true);
    }
};

// -------------------------------------------------------------------------------------------------------------------
// Cleanup

struct AudioIODeviceTypeComparator
{
    static int compareElements (const juce::AudioIODeviceType* d1, const juce::AudioIODeviceType* d2) noexcept
    {
        return d1->getTypeName().compareNatural (d2->getTypeName());
    }
};

static void initJuceDevicesIfNeeded()
{
    static juce::AudioDeviceManager sDeviceManager;

    if (gDeviceTypes.size() != 0)
        return;

    sDeviceManager.createAudioDeviceTypes(gDeviceTypes);

    CARLA_SAFE_ASSERT_RETURN(gDeviceTypes.size() != 0,);

    new JuceCleanup();

    // remove JACK from device list
    for (int i=0, count=gDeviceTypes.size(); i < count; ++i)
    {
        if (gDeviceTypes[i]->getTypeName() == "JACK")
        {
            gDeviceTypes.remove(i, true);
            break;
        }
    }

    AudioIODeviceTypeComparator comp;
    gDeviceTypes.sort(comp);
}

// -------------------------------------------------------------------------------------------------------------------
// Juce Engine

class CarlaEngineJuce : public CarlaEngine,
                        public juce::AudioIODeviceCallback,
                        public juce::MidiInputCallback
{
public:
    CarlaEngineJuce(juce::AudioIODeviceType* const devType)
        : CarlaEngine(),
          juce::AudioIODeviceCallback(),
          fDevice(),
          fDeviceType(devType),
          fMidiIns(),
          fMidiInEvents(),
          fMidiOuts(),
          fMidiOutMutex()
    {
        carla_debug("CarlaEngineJuce::CarlaEngineJuce(%p)", devType);

        // just to make sure
        pData->options.transportMode = ENGINE_TRANSPORT_MODE_INTERNAL;
    }

    ~CarlaEngineJuce() override
    {
        carla_debug("CarlaEngineJuce::~CarlaEngineJuce()");
    }

    // -------------------------------------

    bool init(const char* const clientName) override
    {
        CARLA_SAFE_ASSERT_RETURN(clientName != nullptr && clientName[0] != '\0', false);
        carla_debug("CarlaEngineJuce::init(\"%s\")", clientName);

        if (pData->options.processMode != ENGINE_PROCESS_MODE_CONTINUOUS_RACK && pData->options.processMode != ENGINE_PROCESS_MODE_PATCHBAY)
        {
            setLastError("Invalid process mode");
            return false;
        }

        juce::String deviceName;

        if (pData->options.audioDevice != nullptr && pData->options.audioDevice[0] != '\0')
        {
            deviceName = pData->options.audioDevice;
        }
        else
        {
            const int defaultIndex = fDeviceType->getDefaultDeviceIndex(false);
            juce::StringArray deviceNames(fDeviceType->getDeviceNames());

            if (defaultIndex >= 0 && defaultIndex < deviceNames.size())
                deviceName = deviceNames[defaultIndex];
        }

        if (deviceName.isEmpty())
        {
            setLastError("Audio device has not been selected yet and a default one is not available");
            return false;
        }

        fDevice = fDeviceType->createDevice(deviceName, deviceName);

        if (fDevice == nullptr)
        {
            setLastError("Failed to create device");
            return false;
        }

        juce::StringArray inputNames(fDevice->getInputChannelNames());
        juce::StringArray outputNames(fDevice->getOutputChannelNames());

        if (inputNames.size() < 0 || outputNames.size() <= 0)
        {
            setLastError("Selected device does not have any outputs");
            return false;
        }

        juce::BigInteger inputChannels;
        inputChannels.setRange(0, inputNames.size(), true);

        juce::BigInteger outputChannels;
        outputChannels.setRange(0, outputNames.size(), true);

        juce::String error = fDevice->open(inputChannels, outputChannels, pData->options.audioSampleRate, static_cast<int>(pData->options.audioBufferSize));

        if (error.isNotEmpty())
        {
            setLastError(error.toUTF8());
            fDevice = nullptr;
            return false;
        }

        if (! pData->init(clientName))
        {
            close();
            setLastError("Failed to init internal data");
            return false;
        }

        pData->bufferSize = static_cast<uint32_t>(fDevice->getCurrentBufferSizeSamples());
        pData->sampleRate = fDevice->getCurrentSampleRate();
        pData->initTime(pData->options.transportExtra);

        pData->graph.create(static_cast<uint32_t>(inputNames.size()),
                            static_cast<uint32_t>(outputNames.size()),
                            0, 0);

        fDevice->start(this);

        patchbayRefresh(true, false, false);

        if (pData->options.processMode == ENGINE_PROCESS_MODE_PATCHBAY)
            refreshExternalGraphPorts<PatchbayGraph>(pData->graph.getPatchbayGraph(), false, false);

        callback(true, true,
                 ENGINE_CALLBACK_ENGINE_STARTED,
                 0,
                 pData->options.processMode,
                 pData->options.transportMode,
                 static_cast<int>(pData->bufferSize),
                 static_cast<float>(pData->sampleRate),
                 getCurrentDriverName());
        return true;
    }

    bool close() override
    {
        carla_debug("CarlaEngineJuce::close()");

        bool hasError = false;

        // stop stream first
        if (fDevice != nullptr && fDevice->isPlaying())
            fDevice->stop();

        // clear engine data
        CarlaEngine::close();

        pData->graph.destroy();

        for (LinkedList<MidiInPort>::Itenerator it = fMidiIns.begin2(); it.valid(); it.next())
        {
            MidiInPort& inPort(it.getValue(kMidiInPortFallbackNC));
            CARLA_SAFE_ASSERT_CONTINUE(inPort.port != nullptr);

            inPort.port->stop();
            delete inPort.port;
        }

        fMidiIns.clear();
        fMidiInEvents.clear();

        fMidiOutMutex.lock();

        for (LinkedList<MidiOutPort>::Itenerator it = fMidiOuts.begin2(); it.valid(); it.next())
        {
            MidiOutPort& outPort(it.getValue(kMidiOutPortFallbackNC));
            CARLA_SAFE_ASSERT_CONTINUE(outPort.port != nullptr);

            outPort.port->stopBackgroundThread();
            delete outPort.port;
        }

        fMidiOuts.clear();
        fMidiOutMutex.unlock();

        // close stream
        if (fDevice != nullptr)
        {
            if (fDevice->isOpen())
                fDevice->close();

            fDevice = nullptr;
        }

        return !hasError;
    }

    bool hasIdleOnMainThread() const noexcept override
    {
        return true;
    }

    bool isRunning() const noexcept override
    {
        return fDevice != nullptr && fDevice->isOpen();
    }

    bool isOffline() const noexcept override
    {
        return false;
    }

    EngineType getType() const noexcept override
    {
        return kEngineTypeJuce;
    }

    const char* getCurrentDriverName() const noexcept override
    {
        return fDeviceType->getTypeName().toRawUTF8();
    }

    /*
    float getDSPLoad() const noexcept override
    {
        return 0.0f;
    }
    */

    uint32_t getTotalXruns() const noexcept override
    {
        const int xruns = fDevice->getXRunCount();

        if (xruns <= 0)
          return 0;

        const uint uxruns = static_cast<uint>(xruns);

        if (uxruns <= pData->xruns)
            return 0;

        return uxruns - pData->xruns;
    }

    void clearXruns() const noexcept override
    {
        const int xruns = fDevice->getXRunCount();
        pData->xruns = xruns > 0 ? static_cast<uint32_t>(xruns) : 0;
    }

    bool setBufferSizeAndSampleRate(const uint bufferSize, const double sampleRate) override
    {
        CARLA_SAFE_ASSERT_RETURN(fDevice != nullptr, false);

        juce::StringArray inputNames(fDevice->getInputChannelNames());
        juce::StringArray outputNames(fDevice->getOutputChannelNames());

        if (inputNames.size() < 0 || outputNames.size() <= 0)
        {
            setLastError("Selected device does not have any outputs");
            return false;
        }

        juce::BigInteger inputChannels;
        inputChannels.setRange(0, inputNames.size(), true);

        juce::BigInteger outputChannels;
        outputChannels.setRange(0, outputNames.size(), true);

        // stop stream first
        if (fDevice->isPlaying())
            fDevice->stop();
        if (fDevice->isOpen())
            fDevice->close();

        juce::String error = fDevice->open(inputChannels, outputChannels, sampleRate, static_cast<int>(bufferSize));

        if (error.isNotEmpty())
        {
            setLastError(error.toUTF8());

            // try to roll back
            error = fDevice->open(inputChannels, outputChannels, pData->sampleRate, static_cast<int>(pData->bufferSize));

            // if we failed, we are screwed...
            if (error.isNotEmpty())
            {
                fDevice = nullptr;
                close();
            }

            return false;
        }

        const uint32_t newBufferSize = static_cast<uint32_t>(fDevice->getCurrentBufferSizeSamples());
        const double   newSampleRate = fDevice->getCurrentSampleRate();

        if (carla_isNotEqual(pData->sampleRate, newSampleRate))
        {
            pData->sampleRate = newSampleRate;
            sampleRateChanged(newSampleRate);
        }

        if (pData->bufferSize != newBufferSize)
        {
            pData->bufferSize = newBufferSize;
            bufferSizeChanged(newBufferSize);
        }

        fDevice->start(this);
        return true;
    }

    bool showDeviceControlPanel() const noexcept override
    {
        try {
            return fDevice->showControlPanel();
        } CARLA_SAFE_EXCEPTION_RETURN("showDeviceControlPanel", false);
    }

    // -------------------------------------------------------------------
    // Patchbay

    template<class Graph>
    bool refreshExternalGraphPorts(Graph* const graph, const bool sendHost, const bool sendOSC)
    {
        CARLA_SAFE_ASSERT_RETURN(graph != nullptr, false);

        char strBuf[STR_MAX];

        ExternalGraph& extGraph(graph->extGraph);

        // ---------------------------------------------------------------
        // clear last ports

        extGraph.clear();

        // ---------------------------------------------------------------
        // fill in new ones

        // Audio In
        {
            juce::StringArray inputNames(fDevice->getInputChannelNames());

            for (int i=0, count=inputNames.size(); i<count; ++i)
            {
                PortNameToId portNameToId;
                portNameToId.setData(kExternalGraphGroupAudioIn, uint(i+1), inputNames[i].toRawUTF8(), "");

                extGraph.audioPorts.ins.append(portNameToId);
            }
        }

        // Audio Out
        {
            juce::StringArray outputNames(fDevice->getOutputChannelNames());

            for (int i=0, count=outputNames.size(); i<count; ++i)
            {
                PortNameToId portNameToId;
                portNameToId.setData(kExternalGraphGroupAudioOut, uint(i+1), outputNames[i].toRawUTF8(), "");

                extGraph.audioPorts.outs.append(portNameToId);
            }
        }

        // MIDI In
        {
            juce::Array<juce::MidiDeviceInfo> midiIns(juce::MidiInput::getAvailableDevices());

            for (int i=0, count=midiIns.size(); i<count; ++i)
            {
                const juce::MidiDeviceInfo devInfo(midiIns[i]);

                if (devInfo.name == "a2jmidid - port")
                    continue;

                PortNameToId portNameToId;
                portNameToId.setDataWithIdentifier(kExternalGraphGroupMidiIn, static_cast<uint>(i + 1),
                                                   devInfo.name.toRawUTF8(), devInfo.identifier.toRawUTF8());

                extGraph.midiPorts.ins.append(portNameToId);
            }
        }

        // MIDI Out
        {
            juce::Array<juce::MidiDeviceInfo> midiOuts(juce::MidiOutput::getAvailableDevices());

            for (int i=0, count=midiOuts.size(); i<count; ++i)
            {
                const juce::MidiDeviceInfo devInfo(midiOuts[i]);

                if (devInfo.name == "a2jmidid - port")
                    continue;

                PortNameToId portNameToId;
                portNameToId.setDataWithIdentifier(kExternalGraphGroupMidiOut, static_cast<uint>(i + 1),
                                                   devInfo.name.toRawUTF8(), devInfo.identifier.toRawUTF8());

                extGraph.midiPorts.outs.append(portNameToId);
            }
        }

        // ---------------------------------------------------------------
        // now refresh

        if (sendHost || sendOSC)
        {
            juce::String deviceName(fDevice->getName());

            if (deviceName.isNotEmpty())
                deviceName = deviceName.dropLastCharacters(deviceName.fromFirstOccurrenceOf(", ", true, false).length());

            graph->refresh(sendHost, sendOSC, true, deviceName.toRawUTF8());
        }

        // ---------------------------------------------------------------
        // add midi connections

        for (LinkedList<MidiInPort>::Itenerator it=fMidiIns.begin2(); it.valid(); it.next())
        {
            const MidiInPort& inPort(it.getValue(kMidiInPortFallback));
            CARLA_SAFE_ASSERT_CONTINUE(inPort.port != nullptr);

            bool ok;
            const uint portId = extGraph.midiPorts.getPortIdFromIdentifier(true, inPort.identifier, &ok);
            CARLA_SAFE_ASSERT_UINT_CONTINUE(ok, portId);

            ConnectionToId connectionToId;
            connectionToId.setData(++(extGraph.connections.lastId), kExternalGraphGroupMidiIn, portId, kExternalGraphGroupCarla, kExternalGraphCarlaPortMidiIn);

            std::snprintf(strBuf, STR_MAX-1, "%i:%i:%i:%i", connectionToId.groupA, connectionToId.portA, connectionToId.groupB, connectionToId.portB);
            strBuf[STR_MAX-1] = '\0';

            callback(sendHost, sendOSC,
                     ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED,
                     connectionToId.id,
                     0, 0, 0, 0.0f,
                     strBuf);

            extGraph.connections.list.append(connectionToId);
        }

        fMidiOutMutex.lock();

        for (LinkedList<MidiOutPort>::Itenerator it=fMidiOuts.begin2(); it.valid(); it.next())
        {
            const MidiOutPort& outPort(it.getValue(kMidiOutPortFallback));
            CARLA_SAFE_ASSERT_CONTINUE(outPort.port != nullptr);

            bool ok;
            const uint portId = extGraph.midiPorts.getPortIdFromIdentifier(false, outPort.identifier, &ok);
            CARLA_SAFE_ASSERT_UINT_CONTINUE(ok, portId);

            ConnectionToId connectionToId;
            connectionToId.setData(++(extGraph.connections.lastId), kExternalGraphGroupCarla, kExternalGraphCarlaPortMidiOut, kExternalGraphGroupMidiOut, portId);

            std::snprintf(strBuf, STR_MAX-1, "%i:%i:%i:%i", connectionToId.groupA, connectionToId.portA, connectionToId.groupB, connectionToId.portB);
            strBuf[STR_MAX-1] = '\0';

            callback(sendHost, sendOSC,
                     ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED,
                     connectionToId.id,
                     0, 0, 0, 0.0f,
                     strBuf);

            extGraph.connections.list.append(connectionToId);
        }

        fMidiOutMutex.unlock();

        return true;
    }

    bool patchbayRefresh(const bool sendHost, const bool sendOSC, const bool external) override
    {
        CARLA_SAFE_ASSERT_RETURN(pData->graph.isReady(), false);

        if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
            return refreshExternalGraphPorts<RackGraph>(pData->graph.getRackGraph(), sendHost, sendOSC);

        if (sendHost)
            pData->graph.setUsingExternalHost(external);
        if (sendOSC)
            pData->graph.setUsingExternalOSC(external);

        if (external)
            return refreshExternalGraphPorts<PatchbayGraph>(pData->graph.getPatchbayGraph(), sendHost, sendOSC);

        return CarlaEngine::patchbayRefresh(sendHost, sendOSC, false);
    }

    // -------------------------------------------------------------------

protected:
    void audioDeviceIOCallback(const float** inputChannelData, int numInputChannels, float** outputChannelData,
                               int numOutputChannels, int numSamples) override
    {
        CARLA_SAFE_ASSERT_RETURN(numSamples >= 0,);

        const uint32_t nframes(static_cast<uint32_t>(numSamples));
        const PendingRtEventsRunner prt(this, nframes, true); // FIXME remove dspCalc after updating juce

        // assert juce buffers
        CARLA_SAFE_ASSERT_RETURN(numInputChannels >= 0,);
        CARLA_SAFE_ASSERT_RETURN(numOutputChannels > 0,);
        CARLA_SAFE_ASSERT_RETURN(outputChannelData != nullptr,);
        CARLA_SAFE_ASSERT_RETURN(numSamples == static_cast<int>(pData->bufferSize),);

        // initialize juce output
        for (int i=0; i < numOutputChannels; ++i)
            carla_zeroFloats(outputChannelData[i], nframes);

        // initialize events
        carla_zeroStructs(pData->events.in,  kMaxEngineEventInternalCount);
        carla_zeroStructs(pData->events.out, kMaxEngineEventInternalCount);

        if (fMidiInEvents.mutex.tryLock())
        {
            uint32_t engineEventIndex = 0;
            fMidiInEvents.splice();

            for (LinkedList<RtMidiEvent>::Itenerator it = fMidiInEvents.data.begin2(); it.valid(); it.next())
            {
                const RtMidiEvent& midiEvent(it.getValue(kRtMidiEventFallback));
                CARLA_SAFE_ASSERT_CONTINUE(midiEvent.size > 0);

                EngineEvent& engineEvent(pData->events.in[engineEventIndex++]);

                if (midiEvent.time < pData->timeInfo.frame)
                {
                    engineEvent.time = 0;
                }
                else if (midiEvent.time >= pData->timeInfo.frame + nframes)
                {
                    carla_stderr("MIDI Event in the future!, %i vs %i", engineEvent.time, pData->timeInfo.frame);
                    engineEvent.time = static_cast<uint32_t>(pData->timeInfo.frame) + nframes - 1;
                }
                else
                    engineEvent.time = static_cast<uint32_t>(midiEvent.time - pData->timeInfo.frame);

                engineEvent.fillFromMidiData(midiEvent.size, midiEvent.data, 0);

                if (engineEventIndex >= kMaxEngineEventInternalCount)
                    break;
            }

            fMidiInEvents.data.clear();
            fMidiInEvents.mutex.unlock();
        }

        pData->graph.process(pData, inputChannelData, outputChannelData, nframes);

        fMidiOutMutex.lock();

        if (fMidiOuts.count() > 0)
        {
            uint8_t        size    = 0;
            uint8_t        data[3] = { 0, 0, 0 };
            const uint8_t* dataPtr = data;

            for (ushort i=0; i < kMaxEngineEventInternalCount; ++i)
            {
                const EngineEvent& engineEvent(pData->events.out[i]);

                if (engineEvent.type == kEngineEventTypeNull)
                    break;

                else if (engineEvent.type == kEngineEventTypeControl)
                {
                    const EngineControlEvent& ctrlEvent(engineEvent.ctrl);
                    ctrlEvent.convertToMidiData(engineEvent.channel, data);
                    dataPtr = data;
                }
                else if (engineEvent.type == kEngineEventTypeMidi)
                {
                    const EngineMidiEvent& midiEvent(engineEvent.midi);

                    size = midiEvent.size;

                    if (size > EngineMidiEvent::kDataSize && midiEvent.dataExt != nullptr)
                        dataPtr = midiEvent.dataExt;
                    else
                        dataPtr = midiEvent.data;
                }
                else
                {
                    continue;
                }

                if (size > 0)
                {
                    juce::MidiMessage message(static_cast<const void*>(dataPtr), static_cast<int>(size), static_cast<double>(engineEvent.time)/nframes);

                    for (LinkedList<MidiOutPort>::Itenerator it=fMidiOuts.begin2(); it.valid(); it.next())
                    {
                        MidiOutPort& outPort(it.getValue(kMidiOutPortFallbackNC));
                        CARLA_SAFE_ASSERT_CONTINUE(outPort.port != nullptr);

                        outPort.port->sendMessageNow(message);
                    }
                }
            }
        }

        fMidiOutMutex.unlock();
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* /*device*/) override
    {
    }

    void audioDeviceStopped() override
    {
    }

    void audioDeviceError(const juce::String& errorMessage) override
    {
        callback(true, true, ENGINE_CALLBACK_ERROR, 0, 0, 0, 0, 0.0f, errorMessage.toRawUTF8());
    }

    // -------------------------------------------------------------------

    void handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message) override
    {
        const int messageSize(message.getRawDataSize());

        if (messageSize <= 0 || messageSize > EngineMidiEvent::kDataSize)
            return;

        const uint8_t* const messageData(message.getRawData());

        RtMidiEvent midiEvent;
        midiEvent.time = 0; // TODO

        midiEvent.size = static_cast<uint8_t>(messageSize);

        int i=0;
        for (; i < messageSize; ++i)
            midiEvent.data[i] = messageData[i];
        for (; i < EngineMidiEvent::kDataSize; ++i)
            midiEvent.data[i] = 0;

        fMidiInEvents.append(midiEvent);
    }

    // -------------------------------------------------------------------

    bool connectExternalGraphPort(const uint connectionType, const uint portId, const char* const portName) override
    {
        CARLA_SAFE_ASSERT_RETURN(connectionType != 0 || (portName != nullptr && portName[0] != '\0'), false);
        carla_stdout("CarlaEngineJuce::connectExternalGraphPort(%u, %u, \"%s\")", connectionType, portId, portName);

        switch (connectionType)
        {
        case kExternalGraphConnectionAudioIn1:
        case kExternalGraphConnectionAudioIn2:
        case kExternalGraphConnectionAudioOut1:
        case kExternalGraphConnectionAudioOut2:
            return CarlaEngine::connectExternalGraphPort(connectionType, portId, portName);

        case kExternalGraphConnectionMidiInput: {
            juce::Array<juce::MidiDeviceInfo> midiIns(juce::MidiInput::getAvailableDevices());

            for (int i=0, count=midiIns.size(); i<count; ++i)
            {
                const juce::MidiDeviceInfo devInfo(midiIns[i]);

                if (devInfo.name != portName)
                    continue;

                std::unique_ptr<juce::MidiInput> juceMidiIn(juce::MidiInput::openDevice(devInfo.identifier, this));
                juceMidiIn->start();

                MidiInPort midiPort;
                midiPort.port = juceMidiIn.release();

                std::strncpy(midiPort.name, portName, STR_MAX-1);
                midiPort.name[STR_MAX-1] = '\0';

                std::strncpy(midiPort.identifier, devInfo.identifier.toRawUTF8(), STR_MAX-1);
                midiPort.identifier[STR_MAX-1] = '\0';

                carla_stdout("MIDI CON '%s' '%s'", midiPort.name, midiPort.identifier);

                fMidiIns.append(midiPort);
                return true;
            }

            return false;
        }

        case kExternalGraphConnectionMidiOutput: {
            juce::Array<juce::MidiDeviceInfo> midiOuts(juce::MidiOutput::getAvailableDevices());

            for (int i=0, count=midiOuts.size(); i<count; ++i)
            {
                const juce::MidiDeviceInfo devInfo(midiOuts[i]);

                if (devInfo.name != portName)
                    continue;

                std::unique_ptr<juce::MidiOutput> juceMidiOut(juce::MidiOutput::openDevice(devInfo.identifier));
                juceMidiOut->startBackgroundThread();

                MidiOutPort midiPort;
                midiPort.port = juceMidiOut.release();

                std::strncpy(midiPort.name, portName, STR_MAX-1);
                midiPort.name[STR_MAX-1] = '\0';

                std::strncpy(midiPort.identifier, devInfo.identifier.toRawUTF8(), STR_MAX-1);
                midiPort.identifier[STR_MAX-1] = '\0';

                const CarlaMutexLocker cml(fMidiOutMutex);

                fMidiOuts.append(midiPort);
                return true;
            }

            return false;
        }
        }

        return false;
    }

    bool disconnectExternalGraphPort(const uint connectionType, const uint portId, const char* const portName) override
    {
        CARLA_SAFE_ASSERT_RETURN(connectionType != 0 || (portName != nullptr && portName[0] != '\0'), false);
        carla_debug("CarlaEngineJuce::disconnectExternalGraphPort(%u, %u, \"%s\")", connectionType, portId, portName);

        switch (connectionType)
        {
        case kExternalGraphConnectionAudioIn1:
        case kExternalGraphConnectionAudioIn2:
        case kExternalGraphConnectionAudioOut1:
        case kExternalGraphConnectionAudioOut2:
            return CarlaEngine::disconnectExternalGraphPort(connectionType, portId, portName);

        case kExternalGraphConnectionMidiInput:
            for (LinkedList<MidiInPort>::Itenerator it=fMidiIns.begin2(); it.valid(); it.next())
            {
                MidiInPort& inPort(it.getValue(kMidiInPortFallbackNC));
                CARLA_SAFE_ASSERT_CONTINUE(inPort.port != nullptr);

                if (std::strcmp(inPort.name, portName) != 0)
                    continue;

                inPort.port->stop();
                delete inPort.port;

                fMidiIns.remove(it);
                return true;
            }
            break;

        case kExternalGraphConnectionMidiOutput: {
            const CarlaMutexLocker cml(fMidiOutMutex);

            for (LinkedList<MidiOutPort>::Itenerator it=fMidiOuts.begin2(); it.valid(); it.next())
            {
                MidiOutPort& outPort(it.getValue(kMidiOutPortFallbackNC));
                CARLA_SAFE_ASSERT_CONTINUE(outPort.port != nullptr);

                if (std::strcmp(outPort.name, portName) != 0)
                    continue;

                outPort.port->stopBackgroundThread();
                delete outPort.port;

                fMidiOuts.remove(it);
                return true;
            }
        }   break;
        }

        return false;
    }

    // -------------------------------------

private:
    CarlaScopedPointer<juce::AudioIODevice> fDevice;
    juce::AudioIODeviceType* const fDeviceType;

    struct RtMidiEvents {
        CarlaMutex mutex;
        RtLinkedList<RtMidiEvent>::Pool dataPool;
        RtLinkedList<RtMidiEvent> data;
        RtLinkedList<RtMidiEvent> dataPending;

        RtMidiEvents()
            : mutex(),
              dataPool("RtMidiEvents", 512, 512),
              data(dataPool),
              dataPending(dataPool) {}

        ~RtMidiEvents()
        {
            clear();
        }

        void append(const RtMidiEvent& event)
        {
            mutex.lock();
            dataPending.append(event);
            mutex.unlock();
        }

        void clear()
        {
            mutex.lock();
            data.clear();
            dataPending.clear();
            mutex.unlock();
        }

        void splice()
        {
            if (dataPending.count() > 0)
                dataPending.moveTo(data, true /* append */);
        }
    };

    LinkedList<MidiInPort> fMidiIns;
    RtMidiEvents           fMidiInEvents;

    LinkedList<MidiOutPort> fMidiOuts;
    CarlaMutex              fMidiOutMutex;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineJuce)
};

// -----------------------------------------

namespace EngineInit {

CarlaEngine* newJuce(const AudioApi api)
{
    initJuceDevicesIfNeeded();

    juce::String juceApi;

    switch (api)
    {
    case AUDIO_API_NULL:
    case AUDIO_API_OSS:
    case AUDIO_API_PULSEAUDIO:
        break;
    case AUDIO_API_JACK:
        juceApi = "JACK";
        break;
    case AUDIO_API_ALSA:
        juceApi = "ALSA";
        break;
    case AUDIO_API_COREAUDIO:
        juceApi = "CoreAudio";
        break;
    case AUDIO_API_ASIO:
        juceApi = "ASIO";
        break;
    case AUDIO_API_DIRECTSOUND:
        juceApi = "DirectSound";
        break;
    case AUDIO_API_WASAPI:
        juceApi = "Windows Audio";
        break;
    }

    if (juceApi.isEmpty())
        return nullptr;

    juce::AudioIODeviceType* deviceType = nullptr;

    for (int i=0, count=gDeviceTypes.size(); i < count; ++i)
    {
        deviceType = gDeviceTypes[i];

        if (deviceType == nullptr || deviceType->getTypeName() == juceApi)
            break;
    }

    if (deviceType == nullptr)
        return nullptr;

    deviceType->scanForDevices();

    return new CarlaEngineJuce(deviceType);
}

uint getJuceApiCount()
{
    initJuceDevicesIfNeeded();

    return static_cast<uint>(gDeviceTypes.size());
}

const char* getJuceApiName(const uint uindex)
{
    initJuceDevicesIfNeeded();

    const int index(static_cast<int>(uindex));

    CARLA_SAFE_ASSERT_RETURN(index < gDeviceTypes.size(), nullptr);

    juce::AudioIODeviceType* const deviceType(gDeviceTypes[index]);
    CARLA_SAFE_ASSERT_RETURN(deviceType != nullptr, nullptr);

    return deviceType->getTypeName().toRawUTF8();
}

const char* const* getJuceApiDeviceNames(const uint uindex)
{
    initJuceDevicesIfNeeded();

    const int index(static_cast<int>(uindex));

    CARLA_SAFE_ASSERT_RETURN(index < gDeviceTypes.size(), nullptr);

    juce::AudioIODeviceType* const deviceType(gDeviceTypes[index]);
    CARLA_SAFE_ASSERT_RETURN(deviceType != nullptr, nullptr);

    deviceType->scanForDevices();

    juce::StringArray juceDeviceNames(deviceType->getDeviceNames());
    const int         juceDeviceNameCount(juceDeviceNames.size());

    if (juceDeviceNameCount <= 0)
        return nullptr;

    CarlaStringList devNames;

    for (int i=0; i < juceDeviceNameCount; ++i)
        devNames.append(juceDeviceNames[i].toRawUTF8());

    gDeviceNames = devNames.toCharStringListPtr();

    return gDeviceNames;
}

const EngineDriverDeviceInfo* getJuceDeviceInfo(const uint uindex, const char* const deviceName)
{
    initJuceDevicesIfNeeded();

    const int index(static_cast<int>(uindex));

    CARLA_SAFE_ASSERT_RETURN(index < gDeviceTypes.size(), nullptr);

    juce::AudioIODeviceType* const deviceType(gDeviceTypes[index]);
    CARLA_SAFE_ASSERT_RETURN(deviceType != nullptr, nullptr);

    deviceType->scanForDevices();

    CarlaScopedPointer<juce::AudioIODevice> device(deviceType->createDevice(deviceName, deviceName));

    if (device == nullptr)
        return nullptr;

    static EngineDriverDeviceInfo devInfo = { 0x0, nullptr, nullptr };
    static uint32_t dummyBufferSizes[11]  = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 0 };
    static double   dummySampleRates[14]  = { 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 0.0 };

    // reset
    devInfo.hints = ENGINE_DRIVER_DEVICE_VARIABLE_BUFFER_SIZE | ENGINE_DRIVER_DEVICE_VARIABLE_SAMPLE_RATE;

    // cleanup
    if (devInfo.bufferSizes != nullptr && devInfo.bufferSizes != dummyBufferSizes)
    {
        delete[] devInfo.bufferSizes;
        devInfo.bufferSizes = nullptr;
    }

    if (devInfo.sampleRates != nullptr && devInfo.sampleRates != dummySampleRates)
    {
        delete[] devInfo.sampleRates;
        devInfo.sampleRates = nullptr;
    }

    if (device->hasControlPanel())
        devInfo.hints |= ENGINE_DRIVER_DEVICE_HAS_CONTROL_PANEL;

    juce::Array<int> juceBufferSizes = device->getAvailableBufferSizes();
    if (int bufferSizesCount = juceBufferSizes.size())
    {
        uint32_t* const bufferSizes(new uint32_t[bufferSizesCount+1]);

        for (int i=0; i < bufferSizesCount; ++i)
            bufferSizes[i] = static_cast<uint32_t>(juceBufferSizes[i]);
        bufferSizes[bufferSizesCount] = 0;

        devInfo.bufferSizes = bufferSizes;
    }
    else
    {
        devInfo.bufferSizes = dummyBufferSizes;
    }

    juce::Array<double> juceSampleRates = device->getAvailableSampleRates();
    if (int sampleRatesCount = juceSampleRates.size())
    {
        double* const sampleRates(new double[sampleRatesCount+1]);

        for (int i=0; i < sampleRatesCount; ++i)
            sampleRates[i] = juceSampleRates[i];
        sampleRates[sampleRatesCount] = 0.0;

        devInfo.sampleRates = sampleRates;
    }
    else
    {
        devInfo.sampleRates = dummySampleRates;
    }

    return &devInfo;
}

bool showJuceDeviceControlPanel(const uint uindex, const char* const deviceName)
{
    const int index(static_cast<int>(uindex));

    CARLA_SAFE_ASSERT_RETURN(index < gDeviceTypes.size(), false);

    juce::AudioIODeviceType* const deviceType(gDeviceTypes[index]);
    CARLA_SAFE_ASSERT_RETURN(deviceType != nullptr, false);

    deviceType->scanForDevices();

    CarlaScopedPointer<juce::AudioIODevice> device(deviceType->createDevice(deviceName, deviceName));
    CARLA_SAFE_ASSERT_RETURN(device != nullptr, false);

    return device->showControlPanel();
}

}

// -----------------------------------------

CARLA_BACKEND_END_NAMESPACE
