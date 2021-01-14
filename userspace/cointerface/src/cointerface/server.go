package main

import (
	"cointerface/kubecollect_common"
	"cointerface/kubecollect_tc"
	"errors"
	"fmt"
	"golang.org/x/net/context"
	"google.golang.org/grpc"
	"net"
	"os"
	"os/signal"
	"runtime/debug"
	"sync"
	"time"

	"cointerface/compliance"
	"cointerface/k8s_audit"
	"cointerface/kubecollect"
	"github.com/draios/protorepo/sdc_internal"

	log "github.com/cihub/seelog"
	"github.com/docker/docker/client"
	"github.com/gogo/protobuf/proto"
	"github.com/shirou/gopsutil/process"
)

// Reusing docker clients, so we don't need to reconnect to docker daemon
// for every request and also because connections don't appear to get closed
// when the docker client goes out of scope
// We keep one per version, they're supposed to be thread-safe
var dockerClientMapMutex = &sync.Mutex{}
var dockerClientMap = make(map[string]*client.Client)

func getSysdigRoot() string {
    sysdigRoot := os.Getenv("SYSDIG_HOST_ROOT")
    if sysdigRoot != "" {
        sysdigRoot = sysdigRoot + "/"
    }
    return sysdigRoot
}

func GetDockerClient(ver string) (*client.Client, error) {
	dockerClientMapMutex.Lock()
	if cli, exists := dockerClientMap[ver]; exists {
		dockerClientMapMutex.Unlock()
		return cli, nil
	}

	dockerSock := fmt.Sprintf("unix:///%svar/run/docker.sock", getSysdigRoot())

	cli, err := client.NewClient(dockerSock, ver, nil, nil)
	if err != nil {
		dockerClientMapMutex.Unlock()
		ferr := fmt.Errorf("Could not create docker client: %s", err)
		return nil, ferr
	}
	dockerClientMap[ver] = cli
	dockerClientMapMutex.Unlock()
	return cli, nil
}

type coInterfaceServer struct {
}

func (c *coInterfaceServer) PerformCriCommand(ctx context.Context, cmd *sdc_internal.CriCommand) (*sdc_internal.CriCommandResult, error) {
    log.Debugf("Received cri-o command message: %s", cmd.String())

    cri, err := NewCriClient(fmt.Sprintf("unix:///%s%s", getSysdigRoot(), cmd.GetCriSocketPath()))
    if err != nil {
        log.Errorf("Could not connect to cri-o: %s\n", err)
        return nil, err
    }
	defer cri.Close()

    switch cmd.GetCmd() {
    case sdc_internal.ContainerCmdType_STOP:
        err = cri.StopContainer(cmd.GetContainerId(), 30)

    case sdc_internal.ContainerCmdType_PAUSE:
        err = cri.PauseContainer(cmd.GetContainerId())

    case sdc_internal.ContainerCmdType_UNPAUSE:
        err = cri.UnpauseContainer(cmd.GetContainerId())

    case sdc_internal.ContainerCmdType_KILL:
        err = cri.StopContainer(cmd.GetContainerId(), 0)

    default:
        ferr := fmt.Errorf("Unknown cri-o command %d", int(cmd.GetCmd()))
        log.Errorf(ferr.Error())
        return nil, ferr
    }

    res := &sdc_internal.CriCommandResult{}
	res.Successful = proto.Bool(err == nil)
	if err != nil {
	    log.Errorf("Error while handling cri-o command %d: %s", cmd.GetCmd(), err)
		res.Errstr = proto.String(err.Error())
	}

	log.Debugf("Sending response: %s", res.String())

    return res, nil
}

func (c *coInterfaceServer) PerformDockerCommand(ctx context.Context, cmd *sdc_internal.DockerCommand) (*sdc_internal.DockerCommandResult, error) {
	log.Debugf("Received docker command message: %s", cmd.String())

	cli, err := GetDockerClient("v1.18")
	if err != nil {
		return nil, err
	}

	thirty_secs := time.Second * 30
	switch cmd.GetCmd() {
	case sdc_internal.ContainerCmdType_STOP:
		err = cli.ContainerStop(ctx, cmd.GetContainerId(), &thirty_secs)

	case sdc_internal.ContainerCmdType_PAUSE:
		err = cli.ContainerPause(ctx, cmd.GetContainerId())

	case sdc_internal.ContainerCmdType_UNPAUSE:
		err = cli.ContainerUnpause(ctx, cmd.GetContainerId())

	case sdc_internal.ContainerCmdType_KILL:
		err = cli.ContainerKill(ctx, cmd.GetContainerId(), "SIGKILL")

	default:
		ferr := fmt.Errorf("Unknown docker command %d", int(cmd.GetCmd()))
		log.Errorf(ferr.Error())
		return nil, ferr
	}

	res := &sdc_internal.DockerCommandResult{}
	res.Successful = proto.Bool(err == nil)
	if err != nil {
		res.Errstr = proto.String(err.Error())
	}

	log.Debugf("Sending response: %s", res.String())

	return res, nil
}

func (c *coInterfaceServer) PerformPing(ctx context.Context, cmd *sdc_internal.Ping) (*sdc_internal.Pong, error) {
	log.Debugf("Received ping message: %s", cmd.String())

	res := &sdc_internal.Pong{}
	res.Token = proto.Int64(cmd.GetToken())
	pid := int32(os.Getpid())
	res.Pid = proto.Int32(pid)
	res.MemoryUsed = proto.Uint64(0)

	// Try to get our own process's memory usage. If this results
	// in an error, we still let the ping succeed but use a memory
	// usage of 0.
	self, err := process.NewProcess(pid)
	if err != nil {
		log.Errorf("Could not get process info for self: %s", err)
	} else {
		stat, err := self.MemoryInfo()

		if err != nil {
			log.Errorf("Could not get memory usage for self: %s", err)
		} else {
			res.MemoryUsed = proto.Uint64(stat.RSS / 1024)
		}
	}

	log.Debugf("Sending response: %s", res.String())

	return res, nil
}

func (c *coInterfaceServer) PerformSwarmState(ctx context.Context, cmd *sdc_internal.SwarmStateCommand) (*sdc_internal.SwarmStateResult, error) {
	return getSwarmState(ctx, cmd)
}

// function called when we want to start all informers. The messages returned to this
// stream are only those of the "regular" informers. Messaqes for user events are
// collected via a SEPARATE RPC call to attach to that channel. Because of this, some
// requirements must be satisfied:
// 1) Execution within this stream must exceed the lifetime of the user event stream
// 2) the user event stream should exit if we have not completed setup as a part of this function
// 3) if the user event stream closes prematurely, it does not affect the channel itself
// 4) when THIS stream closes, it must also close the user event channel and stream (if one
//    is attached
func (c *coInterfaceServer) PerformOrchestratorEventsStream(cmd *sdc_internal.OrchestratorEventsStreamCommand,
							    stream sdc_internal.CoInterface_PerformOrchestratorEventsStreamServer) error {
	log.Infof("[PerformOrchestratorEventsStream] Starting orchestrator events stream.")
	log.Debugf("[PerformOrchestratorEventsStream] using options: %v", cmd)

	kubecollect_common.ChannelMutex.Lock()

	// either another stream has attached to this, or a previous one hasn't finished
	// cleaning up. Try again later.
	if (kubecollect_common.InformerChannelInUse) {
		kubecollect_common.ChannelMutex.Unlock()
		log.Errorf("[PerformOrchestratorEventsStream] Error: informer channel in use")
		return errors.New("informer channel in use")
	}

	// this remains true until channels are cleaned up and closed by the informer wg
	kubecollect_common.InformerChannelInUse = true
	kubecollect_common.ChannelMutex.Unlock()

	// Golang's default garbage collection allows for a lot of bloat,
	// so we set a more aggressive garbage collection because the initial
	// list of all k8s components can cause memory to balloon. After startup
	// is complete, set it back to the original value to keep CPU low.
	//
	// This changes the garbage collection value for the entire process.
	// It's possible that another RPC could alter these GC values, so do lots
	// of checking and complain loudly if the values aren't what we expect.
	initGC := int(cmd.GetStartupGc())
	origGC := setGC(initGC)
	// Other RPCs, including earlier orch calls, could have bloated
	// memory usage, so run the GC before we start our initial fetch
	log.Debug("Calling debug.FreeOSMemory()")
	debug.FreeOSMemory()

	ctx, ctxCancel := context.WithCancel(stream.Context())

	// Don't defer ctxCancel() yet
	// with this call, we officially pass the control of the InformerChannelInUse flag to client code.
	// It must be cleared either when context is cancelled or cleaned up, or if, for instance, 
	// the function returns with an error before starting informers
	// Get the arrayChan for sending events to dragent

	var pkg kubecollect_common.KubecollectInterface

	if cmd.GetThinCointerface() == true {
		pkg = kubecollect_tc.KubecollectClientTc{}
	} else {
		pkg = kubecollect.KubecollectClient{}
	}

	// In order to discover kubernetes cidrs, we need to configure
	// the pod prefix names. See dragent.yaml config
	// `network_topology.pod_prefix_for_cidr_retrieval'
	kubecollect_common.SetPodPrefixForCidrRetrieval(cmd.PodPrefixForCidrRetrieval)

	evtArrayChan, fetchDone, err := kubecollect_common.WatchCluster(ctx, cmd, pkg)
	if err != nil {
		log.Errorf("[PerformOrchestratorEventsStream] Error: failure to start informers. Cleaning up")

		// triggers informers to clean up
		ctxCancel()
		cleanupGC(origGC, initGC)
		return err
	}

	rpcDone := make(chan struct{})

	// Reset the GC settings after the initial fetch
	// completes or the RPC exits
	go func() {
		select {
		case <-fetchDone:
			log.Info("Orch events initial fetch complete")
		case <-rpcDone:
			log.Debug("Orch events RPC exiting")
		}

		cleanupGC(origGC, initGC)
	}()

	defer func() {
		log.Infof("[PerformOrchestratorEventsStream] Stream Closing")

		// first cancel the ctx so the stream will close if it hasn't already
		// (aka if we got a channel error). The informers will pick this
		// up and clean up the channels when they're done
		ctxCancel()

		// Close kube client channel to make sure events stream shuts down as well
		kubecollect_common.CloseKubeClient()

		// drain all messages from every queue
		// do event channel too, just in case 
		kubecollect_common.DrainChan(evtArrayChan)

		// notify the GC function so it resets the GC back to normal
		close(rpcDone)
	}()

	log.Infof("[PerformOrchestratorEventsStream] Entering select loop.")

	for {
		select {
		case evtArray, ok := <-evtArrayChan:
			if !ok {
				log.Debugf("[PerformOrchestratorEventsStream] event stream finished")
				return nil
			} else {
				if err := stream.Send(&evtArray); err != nil {
					// If send fails; log error reporting
					log.Errorf("Send Stream response for {%v} failed: %v", evtArray, err)
					return err
				}
			}
		case <-ctx.Done():
			log.Infof("[PerformOrchestratorEventsStream] context cancelled")
			return nil
		}
	}

	return nil
}

var userEventStreamActive bool = false

func (c *coInterfaceServer) PerformOrchestratorEventMessageStream(cmd *sdc_internal.OrchestratorAttachUserEventsStreamCommand,
								  stream sdc_internal.CoInterface_PerformOrchestratorEventMessageStreamServer) error {
	log.Info("[PerformOrchestratorEventMessageStream] Starting orchestrator events stream.")
	log.Debugf("[PerformOrchestratorEventMessageStream] using options: %v", cmd)

	if userEventStreamActive {
		log.Error("[PerformOrchestratorEventMessageStream] previous stream still active, exiting.")
		return errors.New("Couldn't start user events watch")
	}
	userEventStreamActive = true

	log.Debug("Calling debug.FreeOSMemory()")
	debug.FreeOSMemory()

	qlen := cmd.GetUserEventQueueLen()
	if qlen < 1 {
		qlen = 1
	}
	userEventChannel := make(chan sdc_internal.K8SUserEvent, qlen)
	ctx, ctxCancel := context.WithCancel(stream.Context())
	var wg sync.WaitGroup

	defer func() {
		log.Info("[PerformOrchestratorEventMessageStream] Stream Exiting")
		ctxCancel()
		// Drain channel just in case the event sender is blocked
		// Cast it to a receive-only channel before sending to DrainChan
		kubecollect_common.DrainChan((<-chan sdc_internal.K8SUserEvent)(userEventChannel))
		// Not keeping state in globals in the event code, so we might be able
		// to get rid of the wg.Wait() to allow a new watch to start while the old
		// one is cleaning up
		wg.Wait()
		userEventStreamActive = false
	}()

	started := kubecollect.StartUserEventsStream(ctx, &wg, userEventChannel, cmd.GetCollectDebugEvents(), cmd.GetIncludeTypes())
	if !started {
		return errors.New("Couldn't start user events watch")
	}

	log.Info("[PerformOrchestratorEventMessageStream] Entering select loop.")
	for {
		select {
		case evt, ok := <-userEventChannel:
			if !ok {
				log.Info("[PerformOrchestratorEventMessageStream] event stream finished")
				return nil
			} else {
				// log.Debugf("[PerformOrchestratorEventMessageStream] sending event: %v", evt)
				err := stream.Send(&evt)
				if err != nil {
					log.Errorf("Stream response for {%v:%v} failed: %v",
						evt.Obj.GetKind(), evt.Obj.GetUid(), err)
					return err
				}
			}
		case <-stream.Context().Done(): // stream closed by client
			log.Debug("[PerformOrchestratorEventMessageStream] stream closed")
			return nil
		case <-ctx.Done(): // context cancelled
			log.Debug("[PerformOrchestratorEventMessageStream] context cancelled")
			return nil
		case <-time.After(10 * time.Second):
			log.Debug("[PerformOrchestratorEventMessageStream] no events for 10 seconds")
		}
	}
	log.Info("[PerformOrchestratorEventMessageStream] exiting select loop.")

	return nil
}

func (c *coInterfaceServer) PerformSetK8SOption(ctx context.Context, cmd *sdc_internal.K8SOptionCommand) (*sdc_internal.K8SOptionResult, error) {
	var success bool = true
	var errstr string

	log.Debugf("[PerformSetK8sOption] received option: %s: %s", cmd.GetKey(), cmd.GetValue())
	if (cmd.GetKey() == "events") {
		if (cmd.GetValue() == "start") {
			log.Info("[PerformSetK8sOption] starting event export")
			kubecollect.SetEventExport(true)
		} else if (cmd.GetValue() == "stop") {
			log.Info("[PerformSetK8sOption] stopping event export")
			kubecollect.SetEventExport(false)
		} else {
			errstr = "Unknown value " + cmd.GetValue() + " for key " + cmd.GetKey()
			success = false
		}
	} else {
		errstr = "Unknown key " + cmd.GetKey()
		success = false
	}

	res := &sdc_internal.K8SOptionResult{
		Successful: proto.Bool(success),
		Errstr: proto.String(errstr),
	}
	return res, nil
}

// The GC helpers only support PerformOrchestratorEventsStream currently
// Using them with another RPC could lead to races in changing/restoring
func setGC(newGC int) int {
	prevGC := debug.SetGCPercent(newGC)
	log.Debugf("Orch events RPC, setting GC to %v (was %v)",
		newGC, prevGC)

	const defaultGC = 100
	if prevGC != defaultGC {
		log.Warnf("Starting orch events RPC, orig GC was %v instead of %v",
			prevGC, defaultGC)
	}

	return prevGC
}

func cleanupGC(origGC int, initGC int) {
	prevGC := debug.SetGCPercent(origGC)
	log.Debugf("Orch events RPC, setting GC to %v (was %v)",
		origGC, prevGC)

	if prevGC != initGC {
		log.Errorf("Cleaning up orch events RPC, GC val was %v, expected %v",
			prevGC, initGC)
	}

	log.Debug("Calling debug.FreeOSMemory()")
	debug.FreeOSMemory()
}

func startServer(sock string, modulesDir string) int {
	log.Tracef("Starting cointerface server, grpc version %s", grpc.Version)

	// Try to remove any existing socket
	_, err := os.Stat(sock)
	if err == nil {
		log.Debugf("Removing existing socket %s", sock)
		err := os.Remove(sock)

		if err != nil {
			log.Errorf("Could not remove exiting socket %s: %s. Exiting.", sock, err)
			return 1
		}
	}

	listener, err := net.Listen("unix", sock)

	if err != nil {
		log.Criticalf("Could not listen on socket %s: %s", sock, err)
		return (1)
	}

	defer listener.Close()
	defer os.Remove(sock)

	// The following message was provided to Goldman Sachs (Oct 2018). Do not change.
	log.Infof("Listening on %s for messages", sock)

	grpcServer := grpc.NewServer()
	sdc_internal.RegisterCoInterfaceServer(grpcServer, &coInterfaceServer{})
	if err = compliance.Register(grpcServer, modulesDir); err != nil {
		log.Errorf("Could not initialize compliance grpc server: %s. Exiting.", err.Error())
		return 1
	}

	if err = k8s_audit.Register(grpcServer); err != nil {
		log.Errorf("Could not initialize k8s audit grpc server: %s. Exiting.", err.Error())
		return 1
	}

	// Capture SIGINT and exit gracefully
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt)

	go func() {
		for {
			sig := <-signals
			log.Debugf("Received signal %s, closing listener", sig)
			listener.Close()
		}
	}()

	grpcServer.Serve(listener)

	return 0
}

