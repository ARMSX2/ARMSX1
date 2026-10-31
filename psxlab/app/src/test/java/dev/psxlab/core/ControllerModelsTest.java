package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class ControllerModelsTest {
    private int id(PadState state){SerialPad pad=new SerialPad(state);assertTrue(pad.exchange(1).acknowledge());return pad.exchange(0x42).value();}
    @Test public void supportedTypesAdvertiseTheirActualProtocol(){PadState state=new PadState();assertEquals(0x41,id(state));state.startAnalog=true;assertEquals(0x73,id(state));state.controllerType=1;assertEquals(0x41,id(state));state.controllerType=2;assertEquals(0x53,id(state));state.controllerType=3;assertEquals(255,idAbsent(state));}
    private int idAbsent(PadState s){SerialPad p=new SerialPad(s);SerialPad.Reply r=p.exchange(1);assertFalse(r.acknowledge());return r.value();}
    @Test public void digitalControllerDoesNotPretendToAcceptDualShockConfiguration(){PadState s=new PadState();s.controllerType=1;SerialPad p=new SerialPad(s);p.exchange(1);assertFalse(p.exchange(0x43).acknowledge());}
    private int transfer(Bus b,int value,int control){b.write8(0x1f801040,value);b.sio0.tick(1500);int r=b.read8(0x1f801040);b.write16(0x1f80104a,control|16);return r;}
    @Test public void secondControllerAndMemoryCardHaveIndependentAddresses(){try(Gpu gpu=new Gpu(false)){Bus b=new Bus(gpu,new PadState(),false);PadState second=new PadState();second.controllerType=1;second.update(1<<14,128,128,128,128);b.sio0.setPad(1,second);b.sio0.insertCard(1,new MemoryCard(MemoryCard.formatted(),null));b.write16(0x1f801048,13);b.write16(0x1f80104e,0x88);b.write16(0x1f80104a,0x3003);assertEquals(255,transfer(b,1,0x3003));assertEquals(0x41,transfer(b,0x42,0x3003));assertEquals(0x5a,transfer(b,0,0x3003));assertEquals(255,transfer(b,0,0x3003));assertEquals(0xbf,transfer(b,0,0x3003));b.write16(0x1f80104a,0x3001);b.write16(0x1f80104a,0x3003);int[] sent={0x81,0x52,0,0,0,0,0,0,0,0,0,0},expected={255,8,0x5a,0x5d,0,0,0x5c,0x5d,0,0,'M','C'};for(int i=0;i<sent.length;i++)assertEquals("Card response "+i,expected[i],transfer(b,sent[i],0x3003));}}
}
