// SPDX-License-Identifier: GPL-3.0-or-later
package kr.co.iefriends.pcsx2;
import android.os.ParcelFileDescriptor;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.File;
import java.io.FileOutputStream;
import static org.junit.Assert.*;
@RunWith(AndroidJUnit4.class)
public class Ps1BiosFilterTest {
    private static com.armsx2.BiosInfo probe(File file) throws Exception {
        ParcelFileDescriptor descriptor=ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);
        return NativeApp.getBiosInfoFromFd(descriptor.detachFd());
    }
    @Test public void validatesRealPs1AndRejectsOtherFiles() throws Exception {
        File root=new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(),"swan-smoke");
        File real=new File(root,"bios/scph5501.bin");
        assertTrue("User-supplied PS1 test fixture is required",real.isFile());
        com.armsx2.BiosInfo info=probe(real);assertNotNull("Real PS1 BIOS rejected",info);
        assertEquals("Region should be identified from actual BIOS",1,info.region);
        File junk=new File(root,"BIOS-filter-invalid.bin");
        byte[] random=new byte[512*1024];random[100]='S';random[101]='C';random[102]='E';random[103]='A';
        try(FileOutputStream stream=new FileOutputStream(junk)){stream.write(random);}
        assertNull("512 KB with SCEA marker must not pass",probe(junk));
        File other=new File(root,"BIOS-filter-other-system.bin");
        try(FileOutputStream stream=new FileOutputStream(other)){for(int i=0;i<8;i++)stream.write(random);}
        assertNull("4 MB non-PS1 BIOS must not pass",probe(other));
        File empty=new File(root,"BIOS-filter-empty.bin");
        try(FileOutputStream stream=new FileOutputStream(empty)){}
        assertNull(probe(empty));
    }
}
