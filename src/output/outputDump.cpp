#include "../idefix.hpp"

OutputDump::OutputDump(Input &input, DataBlock &data, real t) {
    // Init the output period
    if(input.CheckEntry("Output","dmp")>0) {
        this->tperiod=input.GetReal("Output","dmp",0);
        this->tnext = t;
    }
    else {
        this->tperiod = -1.0; // Disable dump outputs altogether
        this->tnext = 0;
    }

    
    this->dumpFileNumber = 0;

    // Allocate scratch Array
    this->scrch = new real[(data.np_int[IDIR]+IOFFSET)*(data.np_int[JDIR]+JOFFSET)*(data.np_int[KDIR]+KOFFSET)];

    #ifdef WITH_MPI
        Grid *grid = data.mygrid;
        MPI_Datatype realType;

        #ifdef USE_DOUBLE
        realType = MPI_DOUBLE;
        #else
        realType = MPI_SINGLE;
        #endif

        int start[3];
        int size[3];
        int subsize[3];

        // Dimensions for cell-centered fields
        for(int dir = 0; dir < 3 ; dir++) {
            size[2-dir] = grid->np_int[dir];
            start[2-dir] = data.gbeg[dir]-data.nghost[dir];
            subsize[2-dir] = data.np_int[dir];
        }

        MPI_SAFE_CALL(MPI_Type_create_subarray(3, size, subsize, start, MPI_ORDER_C, realType, &this->descC));
        MPI_SAFE_CALL(MPI_Type_commit(&this->descC));

        // Dimensions for face-centered field
        for(int face = 0; face < 3 ; face++) {
            for(int dir = 0; dir < 3 ; dir++) {
                size[2-dir] = grid->np_int[dir];
                start[2-dir] = data.gbeg[dir]-data.nghost[dir];
                subsize[2-dir] = data.np_int[dir];
            }
            // Add the extra guy in the face direction
            size[2-face]++;
            subsize[2-face]++;    // That's actually valid for reading since it involves an overlap of data between procs

            MPI_SAFE_CALL(MPI_Type_create_subarray(3, size, subsize, start, MPI_ORDER_C, realType, &this->descSR[face]));
            MPI_SAFE_CALL(MPI_Type_commit(&this->descSR[face]));

            // Now for writing, it is only the last proc which keeps one additional cell
            if(grid->xproc[face] != grid->nproc[face] - 1  ) subsize[2-face]--;
            MPI_SAFE_CALL(MPI_Type_create_subarray(3, size, subsize, start, MPI_ORDER_C, realType, &this->descSW[face]));
            MPI_SAFE_CALL(MPI_Type_commit(&this->descSW[face]));
        }
    #endif

}

void OutputDump::WriteString(IdfxFileHandler fileHdl, char *str) {
    #ifdef WITH_MPI
        MPI_Status status;
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, str, nameSize, MPI_CHAR, &status));
        }
        offset=offset+nameSize;
    #else
        fwrite (str, nameSize, sizeof(char), fileHdl);
    #endif
}


void OutputDump::WriteSerial(IdfxFileHandler fileHdl, int ndim, int *dim, DataType type, char* name, void* data ) {
    int ntot = 1;   // Number of elements to be written
    int size;

    if(type == DoubleType) size=sizeof(double);
    if(type == SingleType) size=sizeof(float);
    if(type == IntegerType) size=sizeof(int);

    // Write field name

    WriteString(fileHdl, name);

    #ifdef WITH_MPI
        MPI_Status status;
        MPI_Datatype MpiType;

        // Write data type
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, &type, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);

        // Write dimensions
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, &ndim, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);

        for(int n = 0 ; n < ndim ; n++) {
            MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
            if(idfx::prank==0) {
                MPI_SAFE_CALL(MPI_File_write(fileHdl, dim+n, 1, MPI_INT, &status));
            }
            offset=offset+sizeof(int);
            ntot = ntot * dim[n];
        }

        // Write raw data
        if(type == DoubleType) MpiType=MPI_DOUBLE;
        if(type == SingleType) MpiType=MPI_FLOAT;
        if(type == IntegerType) MpiType=MPI_INT;
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));

        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, data, ntot, MpiType, &status));
        }
        // increment offset accordingly
        offset += ntot*size;

    #else
        // Write type of data
        fwrite(&type, 1, sizeof(int), fileHdl);

        // Write dimensions of array
        fwrite(&ndim, 1, sizeof(int), fileHdl);
        for(int n = 0 ; n < ndim ; n++) {
            fwrite(dim+n, 1, sizeof(int), fileHdl);
            ntot = ntot * dim[n];
        }
        
        // Write raw data
        fwrite(data, ntot, size, fileHdl);
    #endif
}

void OutputDump::WriteDistributed(IdfxFileHandler fileHdl, int ndim, int *dim, int *gdim, char* name, IdfxDataDescriptor &descriptor, real* data ) {
    long int ntot = 1;   // Number of elements to be written

    // Define current datatype
    DataType type;
    #ifdef USE_DOUBLE
    type = DoubleType;
    #else
    type = SingleType;
    #endif

     // Write field name
    WriteString(fileHdl, name);

    #ifdef WITH_MPI
        MPI_Status status;
        MPI_Datatype MpiType;
        long int nglob = 1;

        // Write data type
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, &type, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);

        // Write dimensions
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_write(fileHdl, &ndim, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);

        for(int n = 0 ; n < ndim ; n++) {
            MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
            if(idfx::prank==0) {
                MPI_SAFE_CALL(MPI_File_write(fileHdl, gdim+n, 1, MPI_INT, &status));
            }
            offset=offset+sizeof(int);
            ntot = ntot * dim[n];
            nglob = nglob * gdim[n];
        }

        // Write raw data
        if(type == DoubleType) MpiType=MPI_DOUBLE;
        if(type == SingleType) MpiType=MPI_FLOAT;

        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MpiType, descriptor, "native", MPI_INFO_NULL ));
        MPI_SAFE_CALL(MPI_File_write_all(fileHdl, data, ntot, MpiType, MPI_STATUS_IGNORE));
        
        offset=offset+nglob*sizeof(real);

    #else
        // Write type of data
        
        fwrite(&type, 1, sizeof(int), fileHdl);

        // Write dimensions of array (in serial, dim and gdim are identical, so no need to differentiate)
        fwrite(&ndim, 1, sizeof(int), fileHdl);
        for(int n = 0 ; n < ndim ; n++) {
            fwrite(dim+n, 1, sizeof(int), fileHdl);
            ntot = ntot * dim[n];
        }

        // Write raw data
        fwrite(data, ntot, sizeof(real), fileHdl);
    #endif
}

void OutputDump::ReadNextFieldProperties(IdfxFileHandler fileHdl, int &ndim, int *dim, DataType &type, std::string &name) {
    char fieldName[nameSize];
    long int ntot=1;
    #ifdef WITH_MPI
        // Read Name
        MPI_Status status;
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_read(fileHdl, fieldName, nameSize, MPI_CHAR, &status));
        }
        offset=offset+nameSize;
        // Broadcast
        MPI_SAFE_CALL(MPI_Bcast(fieldName, nameSize, MPI_CHAR, 0, MPI_COMM_WORLD));
        name.assign(fieldName,strlen(fieldName));

        // Read Datatype
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_read(fileHdl, &type, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);
        MPI_SAFE_CALL(MPI_Bcast(&type, 1, MPI_INT, 0, MPI_COMM_WORLD));

        // Read Dimensions
        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_read(fileHdl, &ndim, 1, MPI_INT, &status));
        }
        offset=offset+sizeof(int);
        MPI_SAFE_CALL(MPI_Bcast(&ndim, 1, MPI_INT, 0, MPI_COMM_WORLD));

        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_read(fileHdl, dim, ndim, MPI_INT, &status));
        }
        offset=offset+sizeof(int)*ndim;
        MPI_SAFE_CALL(MPI_Bcast(dim, ndim, MPI_INT, 0, MPI_COMM_WORLD));

        
    #else
        // Read name
        fread(fieldName, sizeof(char), nameSize, fileHdl);
        name.assign(fieldName,strlen(fieldName));

        // Read datatype
        fread(&type, sizeof(int), 1, fileHdl);

        // read dimensions
        fread(&ndim, sizeof(int), 1, fileHdl);
        fread(dim, sizeof(int), ndim, fileHdl);
    #endif

}

void OutputDump::ReadSerial(IdfxFileHandler fileHdl, int ndim, int *dim, DataType type, void* data) {
    int size;
    long int ntot=1;
    // Get total size
    for(int i=0; i < ndim; i++) {
        ntot=ntot*dim[i];
        
    }
    if(type == DoubleType) size=sizeof(double);
    if(type == SingleType) size=sizeof(float);
    if(type == IntegerType) size=sizeof(int);

    #ifdef WITH_MPI
        MPI_Status status;
        MPI_Datatype MpiType;

        if(type == DoubleType) MpiType=MPI_DOUBLE;
        if(type == SingleType) MpiType=MPI_FLOAT;
        if(type == IntegerType) MpiType=MPI_INT;

        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, this->offset, MPI_BYTE, MPI_CHAR, "native", MPI_INFO_NULL ));
        if(idfx::prank==0) {
            MPI_SAFE_CALL(MPI_File_read(fileHdl, data, ntot, MpiType, &status));
        }
        offset+= ntot*size;
        MPI_SAFE_CALL(MPI_Bcast(data, ntot, MpiType, 0, MPI_COMM_WORLD));

    #else
        // Read raw data
        fread(data,size,ntot,fileHdl);
    #endif
}

void OutputDump::ReadDistributed(IdfxFileHandler fileHdl, int ndim, int *dim, int *gdim, IdfxDataDescriptor &descriptor, void* data) {
    int size;
    long int ntot=1;
    long int nglob=1;
    // Get total size
    for(int i=0; i < ndim; i++) {
        ntot=ntot*dim[i];
        nglob=nglob*gdim[i];
    }

    #ifdef WITH_MPI
        MPI_Datatype MpiType;

        #ifdef USE_DOUBLE
        MpiType = MPI_DOUBLE;
        #else
        MpiType = MPI_FLOAT;
        #endif

        MPI_SAFE_CALL(MPI_File_set_view(fileHdl, offset, MpiType, descriptor, "native", MPI_INFO_NULL ));
        MPI_SAFE_CALL(MPI_File_read_all(fileHdl, data, ntot, MpiType, MPI_STATUS_IGNORE));
        
        offset=offset+nglob*sizeof(real);
    #else
        // Read raw data
        fread(data,sizeof(real),ntot,fileHdl);
    #endif
}

int OutputDump::Read( Grid& grid, DataBlock &data, TimeIntegrator &tint, OutputVTK& ovtk, int readNumber ) {
    char filename[256];
    int nx[3];
    int nxglob[3];
    std::string fieldName;
    std::string eof ("eof");
    DataType type;
    int ndim;
    IdfxFileHandler fileHdl;

    idfx::pushRegion("OutputDump::Read");

    idfx::cout << "OutputDump::Reading restart file n " << readNumber << "..." << std::flush;

    // Reset timer
    timer.reset();

    // Set filename
    sprintf (filename, "dump.%04d.dmp", readNumber);

    // Make a local image of the datablock
    DataBlockHost dataHost(data);

    // open file
#ifdef WITH_MPI
    MPI_SAFE_CALL(MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_RDONLY | MPI_MODE_UNIQUE_OPEN,MPI_INFO_NULL, &fileHdl));
    this->offset = 0;
#else
    fileHdl = fopen(filename,"rb");
#endif
    // File is open
    // First thing is compare the total domain size
    for(int dir=0 ; dir < 3; dir++) {
    
        ReadNextFieldProperties(fileHdl, ndim, nx, type, fieldName);
        //idfx::cout << "Next field is " << fieldName << " with " << ndim << " dimensions and " << nx[0] << " points " << std::endl; 
        if(ndim>1) IDEFIX_ERROR("Wrong coordinate array dimensions while reading restart dump");
        if(nx[0] != grid.np_int[dir]) {
            idfx::cout << "dir " << dir << ", restart has " << nx[0] << " points " << std::endl;
            IDEFIX_ERROR("Domain size from the restart dump is different from the current one");
        }
        
        // Read coordinates
        ReadSerial(fileHdl, ndim, nx, type, scrch);
        // Todo: check that coordinates are identical
    }
    // Coordinates are ok, load the bulk
    while(true) {
        ReadNextFieldProperties(fileHdl, ndim, nxglob, type, fieldName);
        //idfx::cout << "Next field is " << fieldName << " with " << ndim << " dimensions and ("; 
        //for(int i = 0 ; i < ndim ; i++) idfx::cout << nxglob[i] << " ";
        //idfx::cout << ") points." << std::endl;

        if(fieldName.compare(eof) == 0) break;

        else if(fieldName.compare(0,3,"Vc-") == 0) {
            // Next field is a cell-centered field
            
            // Matching Name is Vc-<<VcName>>
            int nv = -1;
            for(int n = 0 ; n < NVAR; n++) {
                if(fieldName.compare(3,3,data.VcName[n],0,3)==0) nv=n; // Found matching field
            }
            // Load it
            for(int dir = 0 ; dir < 3; dir++) {
                nx[dir] = dataHost.np_int[dir];
            }
            ReadDistributed(fileHdl, ndim, nx, nxglob, descC, scrch);

            if(nv<0) IDEFIX_WARNING("Cannot find a field matching " + fieldName + " in current running code. Skipping.");
            // Load the scratch space in designated field
            
            else {
                for(int k = 0; k < nx[KDIR]; k++) {
                    for(int j = 0 ; j < nx[JDIR]; j++) {
                        for(int i = 0; i < nx[IDIR]; i++) {
                            dataHost.Vc(nv,k+dataHost.beg[KDIR],j+dataHost.beg[JDIR],i+dataHost.beg[IDIR]) = scrch[i + j*nx[IDIR] + k*nx[IDIR]*nx[JDIR]];
                        }
                    }
                }
            }
        }
        else if(fieldName.compare(0,3,"Vs-") == 0) {
            // Next field is a face-centered field
            
            // Matching Name is Vs-<<VcName>>
            #if MHD == YES
                int nv = -1;
                for(int n = 0 ; n < DIMENSIONS; n++) {
                    if(fieldName.compare(3,4,data.VsName[n],0,4)==0) nv=n; // Found matching field
                }
                if(nv<0) IDEFIX_ERROR("Cannot find a field matching " + fieldName + " in current running code.");
                else {
                    // Load it
                    for(int dir = 0 ; dir < 3; dir++) nx[dir] = dataHost.np_int[dir];
                    nx[nv]++;   // Extra cell in the dir direction for cell-centered fields
                    ReadDistributed(fileHdl, ndim, nx, nxglob, descSR[nv], scrch);
                    
                    for(int k = 0; k < nx[KDIR]; k++) {
                        for(int j = 0 ; j < nx[JDIR]; j++) {
                            for(int i = 0; i < nx[IDIR]; i++) {
                                dataHost.Vs(nv,k+dataHost.beg[KDIR],j+dataHost.beg[JDIR],i+dataHost.beg[IDIR]) = scrch[i + j*nx[IDIR] + k*nx[IDIR]*nx[JDIR]];
                            }
                        }
                }
                }
            #else
                IDEFIX_WARNING("Code configured without MHD. Face-centered magnetic field components from the restart dump are skipped");
            #endif
        }
        else if(fieldName.compare("time") == 0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &tint.t);
        }
        else if(fieldName.compare("dt") == 0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &tint.dt);
        }
        else if(fieldName.compare("vtkFileNumber")==0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &ovtk.vtkFileNumber);
        }
        else if(fieldName.compare("vtktnext")==0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &ovtk.tnext);
        }
        else if(fieldName.compare("dumpFileNumber")==0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &this->dumpFileNumber);
        }
        else if(fieldName.compare("dumptnext")==0) {
            ReadSerial(fileHdl, ndim, nxglob, type, &this->tnext);
        }
        else {
            ReadSerial(fileHdl,ndim, nxglob, type, scrch);
            IDEFIX_WARNING("Unknown field "+fieldName+" in restart dump. Skipping.");
        }

    }

    #ifdef WITH_MPI
    MPI_SAFE_CALL(MPI_File_close(&fileHdl));
    #else
    fclose(fileHdl);
    #endif

    // Send to device
    dataHost.SyncToDevice();

    idfx::cout << "done in " << timer.seconds() << " s." << std::endl;
    idfx::cout << "Restarting from t=" << tint.getT() << "." << std::endl;

    idfx::popRegion();

    return(0);
}

int OutputDump::Write( Grid& grid, DataBlock &data, TimeIntegrator &tint, OutputVTK& ovtk) {
    char filename[256];
    char fieldName[nameSize+1]; // +1 is just in case
    int nx[3];
    int nxtot[3];
    #ifdef USE_DOUBLE
    const DataType realType = DoubleType;
    #else
    const DataType realType = SingleType;
    #endif
    IdfxFileHandler fileHdl;

    // Do we need an output?
    if(tint.getT()<this->tnext) return(0);
    if(this->tperiod < 0) return(0);  // negative tperiod means dump outputs are disabled

    idfx::pushRegion("OutputDump::Write");
    this->tnext+= this->tperiod;

    idfx::cout << "OutputDump::Write file n " << dumpFileNumber << "..." << std::flush;

    // Reset timer
    timer.reset();

    // Set filename
    sprintf (filename, "dump.%04d.dmp", dumpFileNumber);
    dumpFileNumber++;   // For next one

    // open file
#ifdef WITH_MPI
    MPI_SAFE_CALL(MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_CREATE | MPI_MODE_RDWR | MPI_MODE_UNIQUE_OPEN,MPI_INFO_NULL, &fileHdl));
    this->offset = 0;
#else
    fileHdl = fopen(filename,"wb");
#endif
    // File is open
    // First thing we need are coordinates: init a host mirror and sync it
    GridHost gridHost(grid);
    gridHost.SyncFromDevice();

    for(int dir = 0; dir < 3 ; dir++) {
        sprintf(fieldName, "x%d",dir+1);
        WriteSerial(fileHdl, 1, &gridHost.np_int[dir], realType, fieldName, (void*) (gridHost.x[dir].data()+gridHost.nghost[dir]));
    }

    // Then write raw data from Vc
    DataBlockHost dataHost(data);
    dataHost.SyncFromDevice();

    for(int nv = 0 ; nv <NVAR ; nv++) {
        sprintf(fieldName,"Vc-%s",data.VcName[nv].c_str());
        // Load the active domain in the scratch space
        for(int i = 0; i < 3 ; i++) {
            nx[i] = dataHost.np_int[i];
            nxtot[i] = grid.np_int[i];
        }

        for(int k = 0; k < nx[KDIR]; k++) {
            for(int j = 0 ; j < nx[JDIR]; j++) {
                for(int i = 0; i < nx[IDIR]; i++) {
                    scrch[i + j*nx[IDIR] + k*nx[IDIR]*nx[JDIR]] = dataHost.Vc(nv,k+dataHost.beg[KDIR],j+dataHost.beg[JDIR],i+dataHost.beg[IDIR]);
                }
            }
        }
        WriteDistributed(fileHdl, 3, nx, nxtot, fieldName, this->descC, scrch);
    }

    #if MHD == YES
        // write staggered field components 
        for(int nv = 0 ; nv <DIMENSIONS ; nv++) {
            sprintf(fieldName,"Vs-%s",data.VsName[nv].c_str());
            // Load the active domain in the scratch space
            for(int i = 0; i < 3 ; i++) {
                nx[i] = dataHost.np_int[i];
                nxtot[i] = grid.np_int[i];
            }
            // If it is the last datablock of the dimension, increase the size by one to get the last active face of the staggered mesh.
            if(grid.xproc[nv] == grid.nproc[nv] - 1  ) nx[nv]++;
            nxtot[nv]++;
            
            for(int k = 0; k < nx[KDIR]; k++) {
                for(int j = 0 ; j < nx[JDIR]; j++) {
                    for(int i = 0; i < nx[IDIR]; i++) {
                        scrch[i + j*nx[IDIR] + k*nx[IDIR]*nx[JDIR] ] = dataHost.Vs(nv,k+dataHost.beg[KDIR],j+dataHost.beg[JDIR],i+dataHost.beg[IDIR]);
                    }
                }
            }
            WriteDistributed(fileHdl, 3, nx, nxtot, fieldName, this->descSW[nv], scrch);
        }
    #endif

    // Write some raw data
    nx[0] = 1;
    sprintf(fieldName, "time");
    WriteSerial(fileHdl, 1, nx, realType, fieldName, &tint.t);
    sprintf(fieldName, "dt");
    WriteSerial(fileHdl, 1, nx, realType, fieldName, &tint.dt);
    sprintf(fieldName, "vtkFileNumber");
    WriteSerial(fileHdl, 1, nx, IntegerType, fieldName, &ovtk.vtkFileNumber);
    sprintf(fieldName, "vtktnext");
    WriteSerial(fileHdl, 1, nx, realType, fieldName, &ovtk.tnext);
    sprintf(fieldName, "dumpFileNumber");
    WriteSerial(fileHdl, 1, nx, IntegerType, fieldName, &this->dumpFileNumber);
    sprintf(fieldName, "dumptnext");
    WriteSerial(fileHdl, 1, nx, realType, fieldName, &this->tnext);
    
    // Write end of file
    scrch[0] = 0.0;
    sprintf(fieldName,"eof");
    nx[0] = 1;
    WriteSerial(fileHdl, 1, nx, realType, fieldName, scrch);

#ifdef WITH_MPI
    MPI_SAFE_CALL(MPI_File_close(&fileHdl));
#else
    fclose(fileHdl);
#endif

    

    idfx::cout << "done in " << timer.seconds() << " s." << std::endl;
    idfx::popRegion();
    // One day, we will have a return code.

    return(0);
}